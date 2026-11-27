/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/run.h"
#include "host/sokol/app/entry.h"
#include "host/sokol/app/pace.h"
#include "host/sokol/app/seam_machine.h"
#include "host/sokol/app/seam.h"
#include "core/aud/mix.h"
#include "core/dap/dbg.h"
#include "core/sys/proc.h"
#include "core/sys/sys.h"
#include "core/vga/vga.h"
#include "core/vga/vga_emu.h"
#include "osal/os.h"
#ifdef EMU_WITH_DEBUGGER
#include "core/dap/dap.h"
#endif
#ifndef __EMSCRIPTEN__
#include "osal/thread.h"
#define RUN_THREAD 1
#else
#include "host/web/link.h"
#define RUN_THREAD 0
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static struct
{
    bool exit_on_halt;
    bool unpaced;
    pace_t pace;
    uint64_t owed_to;   /* the time up to which the machine has been owed */
    uint64_t tick_at;   /* the last display callback's time, 0 before one */
    bool tick_paced;    /* whether its present waited for the display */
    bool held;          /* a debugger held the last frame */
    bool done;          /* the last picture is out; no more frames */
    bool ticked;        /* a display callback came since the last step */
    uint32_t tick_last; /* the newest display callback taken */
    uint64_t bench_ns, bench_from;
} run;

/* ---------------- frames ----------------
 *
 * With a thread, three frames: one for each side and one between them. The
 * machine draws into its own, and when a frame is whole it trades that for
 * the one between, which is then the newest; the window trades its own for the
 * one between when that is newer. Neither side waits for the other, and a frame
 * the window holds is never drawn into. vga draws every row of the canvas in
 * each frame, so a frame needs nothing of the one drawn into it before. A
 * frame a debugger stopped part way is never traded, and nothing is traded
 * after the last picture. Without a thread the window takes each frame on the
 * thread that drew it, and one frame serves: the caller's. On the web the
 * machine is in a worker and the window on the page, each with a frame of its
 * own, and a finished frame goes to the page as a copy (host/web/link.h). */

#define RUN_PIXELS (VGA_MAX_WIDTH * VGA_MAX_HEIGHT)
#define RUN_FRESH 4 /* on the frame between: the machine has traded since the window */

static run_frame_t run_slot[3];
static int run_back;          /* the machine's */
static _Atomic int run_ready; /* between, with RUN_FRESH */
static int run_front;         /* the window's */

/* The machine's frame takes the shape it was drawn in, and is traded. The
 * exchange orders the frame's pixels and shape before the window's. */
static void run_publish(bool last)
{
    run_frame_t *f = &run_slot[run_back];
    vga_canvas_size(&f->canvas_w, &f->canvas_h);
    vga_console_size(&f->console_w, &f->console_h);
    f->last = last;
#if RUN_THREAD
    run_back = atomic_exchange(&run_ready, run_back | RUN_FRESH) & 3;
    vga_set_framebuffer((uint32_t *)run_slot[run_back].pixels);
#else
    link_post_frame(f);
#endif
}

#if !RUN_THREAD
static bool run_web_fresh;

uint32_t *run_web_frame_pixels(void)
{
    return (uint32_t *)run_slot[0].pixels;
}

void run_web_frame_done(int w, int h, int cw, int ch, bool last)
{
    run_frame_t *f = &run_slot[0];
    f->canvas_w = w;
    f->canvas_h = h;
    f->console_w = cw;
    f->console_h = ch;
    f->last = last;
    run_web_fresh = true;
}

const run_frame_t *run_frame(bool *fresh)
{
    *fresh = run_web_fresh;
    run_web_fresh = false;
    return &run_slot[0];
}
#else
const run_frame_t *run_frame(bool *fresh)
{
    *fresh = (atomic_load(&run_ready) & RUN_FRESH) != 0;
    if (*fresh)
        run_front = atomic_exchange(&run_ready, run_front) & 3;
    return &run_slot[run_front];
}
#endif

/* ---------------- the lock and the thread ---------------- */

static _Atomic uint64_t run_machine;

static uint32_t run_ticks_sent;        /* the window's */
static _Atomic uint32_t run_ticks_run; /* the newest tick the machine has run */

#if RUN_THREAD
/* Made before the window opens, and never freed: the debug adapter's thread
 * may wait on the lock to the end of the process. */
static os_lock_t *run_mutex;
static os_wake_t *run_machine_wake, *run_window_wake;
static os_thread_t *run_thread;
static _Atomic bool run_stopping;
/* Those waiting for the lock. A lock is not fair, and the machine would take
 * it straight back, so it lets them have it first. */
static _Atomic int run_waiting;
#endif

static void run_take(void);

#if !RUN_THREAD
/* The worker's clock starts at another moment than the page's, which stamps
 * the display callbacks. */
static int64_t run_web_skew;
#endif

void run_lock(void)
{
#if RUN_THREAD
    if (!run_mutex)
        return;
    atomic_fetch_add(&run_waiting, 1);
    os_lock(run_mutex);
    atomic_fetch_sub(&run_waiting, 1);
#endif
}

void run_unlock(void)
{
#if RUN_THREAD
    if (run_mutex)
        os_unlock(run_mutex);
#endif
}

void run_wake(void)
{
#if RUN_THREAD
    if (run_machine_wake)
        os_wake_signal(run_machine_wake);
#endif
}

/* Between its steps and between frames the machine takes what the window
 * sent, so an event lands no later than it did when the window reached in
 * itself, and lets a debugger waiting for the lock have it. Each waiter
 * takes it and lets it go quickly, so the machine waits on them only that
 * long. */
static void run_pause(void)
{
    if (seam_machine_waiting())
        run_take();
#if RUN_THREAD
    if (!atomic_load_explicit(&run_waiting, memory_order_relaxed))
        return;
    os_unlock(run_mutex);
    while (atomic_load(&run_waiting))
        os_thread_yield();
    os_lock(run_mutex);
#endif
}

/* ---------------- the machine's side ---------------- */

/* The time to `at` is owed to the machine, once. */
static void run_owe_to(uint64_t at)
{
    if (at > run.owed_to)
    {
        pace_credit(&run.pace, at - run.owed_to);
        run.owed_to = at;
    }
}

static void run_bench(void)
{
    const uint64_t now = os_mono_ns();
    if (!run.bench_ns || now - run.bench_from < run.bench_ns)
        return;
    const double secs = (double)(now - run.bench_from) / 1e9;
    fprintf(stderr, "EMU_BENCH: %lu VGA frames in %.3fs = %.1f Hz\n",
            vga_frame_count(), secs, (double)vga_frame_count() / secs);
    run.bench_ns = 0;
    const uint8_t closed = 0;
    seam_to_window(SEAM_QUIT, &closed, 1);
}

/* One frame, false when a debugger holds the machine. The frame that saw the
 * program exit drew the lines above the beam before the program's last
 * writes, so under exit_on_halt one more frame is drawn after it to be the
 * last picture, and the machine runs no more. The drop-a-ROM prompt holds the
 * CPU with no program loaded, and that halt is no program exiting. A drop is
 * a message the machine takes on this thread, so it cannot land between the
 * halt and machine_menu_up. */
static bool run_one(void)
{
    const uint64_t t0 = os_mono_ns();
    const bool whole = vga_run_frame();
    if (whole)
    {
        const bool last = run.exit_on_halt && proc_exited() && !machine_menu_up();
        if (last)
            vga_run_frame();
        run_publish(last);
        run.done = last;
    }
    atomic_fetch_add(&run_machine, os_mono_ns() - t0);
    run.held = !whole;
    run_bench();
    return whole;
}

/* The frames the debt makes due. A debugger hold clears the debt, so the
 * machine does not run a burst of catch-up frames when it resumes. */
static void run_due(void)
{
    while (!run.done && pace_due(&run.pace))
    {
        if (!run_one())
        {
            run.pace.debt_ns = 0;
            break;
        }
        pace_ran(&run.pace);
        run_pause();
    }
}

/* A display callback, at its own time: the time to it is owed and the display
 * is measured, as when the callback did it itself. */
static void run_tick(const seam_tick_t *in)
{
    seam_tick_t at = *in;
#if !RUN_THREAD
    /* A callback from before the worker's clock began is at its start. */
    const int64_t worker_at = (int64_t)at.at + run_web_skew;
    at.at = worker_at > 0 ? (uint64_t)worker_at : 0;
#endif
    const seam_tick_t *t = &at;
    run_owe_to(t->at);
    pace_add(&run.pace, t->dt, t->paced);
    run.tick_at = t->at;
    run.tick_paced = t->paced;
    run.ticked = true;
    run.tick_last = t->n;
    aud_set_speed(pace_speed(&run.pace));
}

/* What the window sent, in order: its events and its display callbacks. */
static void run_take(void)
{
    seam_msg_t m;
    while (seam_machine_take(&m))
    {
        if (m.kind == SEAM_TICK)
            run_tick(m.body);
        else
            machine_take(&m);
    }
}

/* When the next frame comes due: at the first display callback that owes
 * enough, while the display paces, and on the wall clock otherwise. A callback
 * that does not come by then, a millisecond's grace past it, is a window held
 * up, in a present or a move, and the machine runs the frame itself. */
static uint64_t run_due_at(void)
{
    const int64_t period = run.pace.period_ns ? run.pace.period_ns : PACE_FRAME_NS;
    const int64_t lead = period < PACE_FRAME_NS ? period : PACE_FRAME_NS;
    const int64_t short_ns = PACE_FRAME_NS - lead / 2 - PACE_EDGE_NS - run.pace.debt_ns;
    const uint64_t wall = run.owed_to + (uint64_t)(short_ns > 0 ? short_ns : 0);
    if (!run.tick_paced || !run.tick_at)
        return wall;
    uint64_t at = run.tick_at + (uint64_t)period;
    while (at < wall)
        at += (uint64_t)period;
    return at + PACE_EDGE_NS;
}

/* A display that has not called back for two of its periods has stopped, a
 * window held up in a present or a move, and is no longer measured: the
 * machine runs on the wall clock at the wall clock's speed until it calls
 * again. */
static void run_display_stopped(uint64_t now)
{
    const int64_t period = run.pace.period_ns ? run.pace.period_ns : PACE_FRAME_NS;
    if (!run.tick_paced || now - run.tick_at < 2 * (uint64_t)period)
        return;
    run.tick_paced = false;
    run.pace.follows = false;
    aud_set_speed(1);
}

/* Whatever is due now, and how long until there is more. A frame a debugger
 * let go runs at once, rather than waiting for what the hold cleared. */
static uint64_t run_frames(uint64_t now, bool ticked)
{
    if (run.held && !(dbg_is_stopped() && sys_running()) && run.pace.debt_ns < PACE_FRAME_NS)
        run.pace.debt_ns = PACE_FRAME_NS;
    if (run.done)
        return PACE_FRAME_NS;
    if (run.unpaced)
    {
        /* A frame each step, and the next step at once. */
        run.pace.debt_ns = 0;
        return run_one() ? 0 : PACE_FRAME_NS;
    }
    if (!ticked && now >= run_due_at())
    {
        run_display_stopped(now);
        run_owe_to(now);
    }
    run_due();
    if (run.held || run.done)
        return PACE_FRAME_NS;
    const uint64_t at = run_due_at();
    return at > now ? at - now : 0;
}

/* What the window sent is taken in order, the frames its display callbacks
 * and the wall clock make due are run, and only then are the callbacks marked
 * run, so a window waiting for one finds its frame. Last the window is told
 * what changed. */
static uint64_t run_step(uint64_t now)
{
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dap_pump();
#endif
    run.ticked = false;
    run_take();
    const bool ticked = run.ticked;
    const uint64_t wait = run_frames(now, ticked);
    if (run.ticked)
        atomic_store(&run_ticks_run, run.tick_last);
    machine_status(run.pace.follows, run.pace.period_ns ? run.pace.period_ns : PACE_FRAME_NS);
    return wait;
}

#if RUN_THREAD
static void run_main(void *arg)
{
    (void)arg;
    os_lock(run_mutex);
    while (!atomic_load(&run_stopping))
    {
        const uint64_t wait = run_step(os_mono_ns());
        os_unlock(run_mutex);
        os_wake_signal(run_window_wake);
#ifdef EMU_WITH_DEBUGGER
        dap_send();
#endif
        if (wait)
            os_wake_wait(run_machine_wake, wait);
        while (atomic_load(&run_waiting))
            os_thread_yield();
        os_lock(run_mutex);
    }
    os_unlock(run_mutex);
}
#endif

#if !RUN_THREAD
/* The page sends no display callbacks before the worker's machine is up:
 * they would only pile up in its queue, and owe it nothing. */
static bool run_web_running;

void run_web_up(void)
{
    run_web_running = true;
}
#endif

void run_display(uint64_t now, uint64_t dt, bool paced, uint64_t room)
{
#if !RUN_THREAD
    if (!run_web_running)
        return;
#endif
    const uint32_t n = ++run_ticks_sent;
    const seam_tick_t t = {.at = now, .dt = dt, .n = n, .paced = paced};
    /* A queue the machine has not emptied drops this one: the next owes the
     * time it missed. */
    seam_to_machine(SEAM_TICK, &t, sizeof t);
#if RUN_THREAD
    if (run_thread)
    {
        for (uint64_t at = now; at - now < room; at = os_mono_ns())
        {
            if (paced ? (int32_t)(atomic_load(&run_ticks_run) - n) >= 0
                      : (atomic_load(&run_ready) & RUN_FRESH) != 0)
                return;
            os_wake_wait(run_window_wake, room - (at - now));
        }
        return;
    }
    /* No thread: the callback runs the frames due at it, and under --phi2 0
     * as many as a frame's time holds. The lock is taken all the same, for
     * the debug adapter's thread. */
    run_lock();
    do
        run_step(os_mono_ns());
    while (run.unpaced && !run.held && !run.done && os_mono_ns() - now < VGA_FRAME_NS);
    run_unlock();
#ifdef EMU_WITH_DEBUGGER
    dap_send();
#endif
#endif
    /* On the web the machine is the worker's (host/web/link.h), and the page
     * never waits for it. */
    (void)room;
}

#if !RUN_THREAD
void run_web_clock(int64_t page_minus_worker_ns)
{
    run_web_skew = page_minus_worker_ns;
}

uint64_t run_web_step(void)
{
    const uint64_t now = os_mono_ns();
    uint64_t wait;
    do
        wait = run_step(os_mono_ns());
    while (run.unpaced && !run.held && !run.done && os_mono_ns() - now < VGA_FRAME_NS);
    return wait;
}
#endif

bool run_quit_asked(void)
{
    return run_slot[run_front].last;
}

uint64_t run_machine_ns(void)
{
    return atomic_load(&run_machine);
}

void run_set_unpaced(bool on)
{
    run.unpaced = on;
}

void run_setup(void)
{
#if RUN_THREAD
    if (run_mutex)
        return;
    run_mutex = os_lock_new();
    run_machine_wake = os_wake_new();
    run_window_wake = os_wake_new();
    seam_set_wake(run_wake);
#endif
#ifdef EMU_WITH_DEBUGGER
    dap_set_host(run_lock, run_unlock, run_wake);
#endif
}

void run_prepare(uint32_t *fb, bool exit_on_halt)
{
    run_setup();
    run.exit_on_halt = exit_on_halt;
    run_slot[0].pixels = fb;
    vga_canvas_size(&run_slot[0].canvas_w, &run_slot[0].canvas_h);
    vga_console_size(&run_slot[0].console_w, &run_slot[0].console_h);
    run_back = run_front = 0;
    atomic_store(&run_ready, 0);
    vga_set_framebuffer(fb);
    const char *bench = getenv("EMU_BENCH_MS");
    run.bench_ns = bench ? (uint64_t)(atof(bench) * 1000000.0) : 0;
}

void run_start(void)
{
    run.owed_to = run.bench_from = os_mono_ns();
#if RUN_THREAD
    /* The two more frames a thread needs, shaped as the first. */
    bool frames = true;
    for (int i = 1; i < 3; i++)
    {
        uint32_t *px = calloc(RUN_PIXELS, sizeof *px);
        frames = frames && px;
        run_slot[i] = run_slot[0];
        run_slot[i].pixels = px;
    }
    if (frames && run_mutex && run_machine_wake && run_window_wake)
    {
        run_back = 1;
        atomic_store(&run_ready, 2);
        run_front = 0;
        vga_set_framebuffer((uint32_t *)run_slot[run_back].pixels);
        vga_set_pause(run_pause);
        run_thread = os_thread_start(run_main, NULL);
    }
    if (!run_thread)
    {
        vga_set_pause(NULL);
        run_back = run_front = 0;
        atomic_store(&run_ready, 0);
        vga_set_framebuffer((uint32_t *)run_slot[0].pixels);
        fprintf(stderr, "rp6502-emu: no thread for the machine; the window runs it\n");
    }
#endif
}

void run_stop(void)
{
#if RUN_THREAD
    if (run_thread)
    {
        atomic_store(&run_stopping, true);
        os_wake_signal(run_machine_wake);
        os_thread_join(run_thread);
        run_thread = NULL;
        vga_set_pause(NULL);
    }
    /* Kept to the end of the process: what tears the machine down runs alone,
     * and the debug adapter's thread, which outlives the window, waits. */
    run_lock();
#endif
}
