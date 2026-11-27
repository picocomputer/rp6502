/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "osal/dir.h"
#include "osal/os.h"
#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/run.h"
#include "host/sokol/app/pace.h"
#include "host/host.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/util/sokol_letterbox.h"
#include "sokol/util/sokol_debugtext.h"
#include "sokol/util/sokol_gl.h"
#ifdef EMU_WITH_DEBUGGER
#include "host/sokol/dbg/dbgui.h"
#include "core/dap/dap.h"
#endif
#include "host/sokol/app/entry.h"
#include "host/sokol/app/prompt.h"
#include "host/sokol/app/icon.h"
#include "host/sokol/app/input.h"
#include "host/sokol/app/evq_machine.h"
#include "host/sokol/app/evq.h"
#ifdef RP6502_PAD_HOST
#include "host/sokol/app/gamepad.h"
#endif
#include "core/sys/version.h"
#include "core/sys/debug_log.h"
#include "core/aud/mix.h"
#include "core/dap/dbg.h"
#include "core/sys/proc.h"
#include "core/vga/vga_emu.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static struct
{
    bool exit_on_halt;
    int title_variant;
} app;

/* Read in app_cleanup, after the machine's thread is gone. */
static bool app_quit_asked;
static bool app_broke;
/* The window's close was asked for and the machine let it close, a break. */
static bool app_closed;

static bool (*app_break_asked)(void);
static void (*app_break_leave)(void);

void app_set_break(bool (*asked)(void), void (*leave)(void))
{
    app_break_asked = asked;
    app_break_leave = leave;
}

void app_set_unpaced(bool on)
{
    run_set_unpaced(on);
}

/* The window closes at the end of this callback. */
static void app_quit(void)
{
    if (!app_quit_asked)
    {
        app_quit_asked = true;
        sapp_request_quit();
    }
}

static void app_serve(void);

static void app_flag(evq_kind_t kind, bool on)
{
    const uint8_t b = on;
    evq_to_machine(kind, &b, 1);
}

/* Whether the last present waited for the display, and when the callbacks came
 * and went, for the display ticks the machine is paced by (run.h). */
static uint64_t entered_ns, returned_ns;
static bool paced, shown_new;
/* How long the window's own work after its wait for the machine took last
 * time, which the wait leaves room for before the display wants the next
 * present. */
static uint64_t work_ns;
#define WORK_MARGIN_NS 1000000
/* Callbacks whose present does not wait, and every how many of them one goes
 * at once: a half second's. */
static unsigned unpaced_presents;
#define UNPACED_PROBE 30
/* A present that waits for the display holds the thread for milliseconds and
 * one that waits for nothing returns in microseconds, so two milliseconds
 * between the stamp taken before the upload and the next callback tells the
 * two apart. */
#define PRESENT_WAIT_NS 2000000

/* The title says what the window is doing with the pointer, and whether the
 * program has stopped, as the machine last told it. */
static int title_variant(void)
{
    const uint8_t flags = input_status()->event.flags;
    if (flags & EVENT_EXITED)
        return 1;
    if ((flags & EVENT_MOUSE_MAPPED) && input_mouse_locked())
        return 3;
    if ((flags & EVENT_MOUSE_MAPPED) && !(flags & EVENT_TO_TABLET))
        return 2;
    return 0;
}

static void update_title(int v)
{
    static const char *const titles[] = {
        "Picocomputer 6502",
        "Picocomputer 6502 (stopped)",
        "Picocomputer 6502  -  click to capture mouse",
        "Picocomputer 6502  -  Esc releases mouse",
    };
    if (v != app.title_variant)
    {
        app.title_variant = v;
        sapp_set_window_title(titles[v]);
    }
}

#ifndef __EMSCRIPTEN__
/* Called on the sound device's own thread (entry.h). It makes what it plays
 * on the spot, from what the frames have stamped, at the device's rate. The
 * web player has its own sound (host/web/audio.c). */
static void app_sound(float *dst, int frames, uint32_t rate)
{
    aud_set_sink_rate(rate);
    aud_render(dst, frames);
}
#endif

void app_init(void)
{
#ifndef __EMSCRIPTEN__
    /* A web page sets its own tab icon. */
    sapp_set_icon(icon_desc());
#endif
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = app_log,
    });
#ifndef __EMSCRIPTEN__
    if (aud_enabled())
    {
        aud_set_streaming(true);
        host_audio_open(app_sound);
    }
#endif
    gfx_setup();
    host_window_init();
    input_locks();
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_init(sapp_dpi_scale());
#endif
    machine_set_window(true);
    if (host_window_menu_active())
        app_flag(EVQ_MENU, true);
    /* Last, once all the machine reaches is up. */
    run_start();
}

void app_frame(void)
{
    const uint64_t now = os_mono_ns();
    /* Once the display has been seen to pace the callbacks it keeps that
     * standing while frames are still coming, because a callback whose own work
     * fills the display period leaves the present nothing left to wait for;
     * and while the machine follows the display, whose callbacks at 240 Hz are
     * so close that one late leaves the next present little to wait for. A
     * display that stops pacing is seen by the pace first. */
    paced = !returned_ns || now - returned_ns >= PRESENT_WAIT_NS || (paced && (shown_new || input_status()->follows));
    const uint64_t dt = entered_ns ? now - entered_ns : VGA_FRAME_NS;
    entered_ns = now;

    /* What the frames due at this callback read is sent first, so they see it
     * at the age the events before the callback have: the lock keys and the
     * gamepads. */
    app_serve();
#ifdef RP6502_PAD_HOST
    gamepad_input_poll();
#endif
    input_locks();

    /* The machine runs the frames due, and the window waits for them as long
     * as it can and still make the display's next present. A present that
     * does not wait keeps no time, so the window keeps a frame's. */
    uint64_t room;
    if (paced)
    {
        const int64_t period = input_status()->period_ns ? input_status()->period_ns : PACE_FRAME_NS;
        const int64_t left = period - (int64_t)work_ns - WORK_MARGIN_NS;
        room = (uint64_t)(left > period / 4 ? left : period / 4);
    }
    else if (++unpaced_presents % UNPACED_PROBE == 0)
        /* A present made at the machine's rate never waits on a display
         * faster than it, so a display that does pace would never be seen
         * to: now and then one goes straight after the last, to find out. */
        room = 0;
    else
    {
        /* The window shows each new frame as the machine finishes it, but
         * no sooner than half a frame after the last callback, which bounds
         * it under --phi2 0. It does not wait on its own clock, which would
         * line its present up with the display's and leave the present
         * nothing to wait for. */
#ifndef __EMSCRIPTEN__
        const uint64_t soonest = now - dt + VGA_FRAME_NS / 2;
        if (soonest > now)
            os_sleep_ns(soonest - now);
#endif
        room = VGA_FRAME_NS;
    }
    run_display(now, dt, paced, room);
    const uint64_t waited = os_mono_ns();

    app_serve();
    gfx_set_frame(run_frame(&shown_new));
    input_pointer_task();
    /* The machine's last picture under exit_on_halt is the frame just taken,
     * so it is presented before the window closes. */
    const bool quit = run_quit_asked();
#ifdef EMU_WITH_DEBUGGER
    /* The debugger windows are built before the upload below, because
     * dbgui_canvas_rect reports the dockspace central node only after a
     * dbgui_draw and the canvas viewport is fitted into that rect.
     *
     * A dpi_scale of 1.0 makes ImGui units framebuffer pixels, so the UI scale,
     * rather than a fractional display scale, sizes the bitmap font, and at a
     * whole-multiple UI scale each texel lands on one pixel. */
    if (dbg_is_active())
    {
        /* The debugger's windows read the machine as it stands, the one thing
         * that takes the machine's lock (run.h). */
        run_lock();
        dbgui_new_frame(sapp_width(), sapp_height(), (double)dt / 1e9, 1.0f);
        dbgui_draw();
        run_unlock();
    }
#endif
    /* After dbgui_draw, because ImGui knows which item the pointer is over only
     * once the windows for this frame are built. */
    input_cursor_task();
    input_apply();
    update_title(title_variant());
    if (quit)
        app_quit();
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active() && dap_quit_requested())
        app_quit();
#endif
    /* A console break is latched by the host's break handler and answered here,
     * because the machine has to be taken down by the loop that shows it, and
     * because this runs even while a debugger is holding the machine. */
    if (app_break_asked && app_break_asked() && !app_broke)
    {
        app_broke = true;
        app_quit();
    }
    gfx_canvas_changed();

    /* The stamp is taken before the upload because on Mesa DRI3 the wait for
     * the display happens on the next GL write rather than in the swap. Nothing
     * from here on reaches the machine, so a present the compositor holds up
     * holds up only the window. */
    returned_ns = os_mono_ns();
    work_ns = returned_ns - waited;
    gfx_upload(shown_new);
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_flush();
#endif
    gfx_begin_pass();
    gfx_blit();
    host_window_menu_draw();
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_render();
#endif
    gfx_end_pass();
}

/* The desktop's window and the web player both gain and lose focus. */
bool host_focusable(void)
{
    return true;
}

#ifndef __EMSCRIPTEN__
bool host_mac_keys(void)
{
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}
#endif

/* What the machine sent the window, taken at every event and frame: what it
 * needs to take the next event (input.h), and what it asked of the host. The
 * clipboard is the desktop's while the window is up, and only the window's
 * thread may reach it, at a moment the machine is never waiting on, because
 * the OS may keep a clipboard call waiting on another program. A copy is put
 * there as it comes, before a paste's ask that follows it, so a cut and then
 * a paste paste what was cut. sokol hands back the text it last had when the
 * desktop's is too big for its buffer, so then there is none. */
#ifndef __EMSCRIPTEN__
static bool app_clipboard_too_big;
#endif

static void app_serve(void)
{
    evq_msg_t m;
    while (evq_window_take(&m))
        switch (m.kind)
        {
        case EVQ_STATUS:
            input_set_status(m.body);
            break;
#ifndef __EMSCRIPTEN__
        case EVQ_COPY:
            sapp_set_clipboard_string(m.body);
            break;
        case EVQ_ASK:
        {
            app_clipboard_too_big = false;
            const char *text = sapp_get_clipboard_string();
            evq_text_to_machine(EVQ_CLIPBOARD, app_clipboard_too_big ? "" : text);
            break;
        }
        case EVQ_BOOTED:
            prompt_booted();
            break;
#else
        case EVQ_COPY:
            entry_clipboard_write(m.body);
            break;
#endif
        case EVQ_QUIT:
            app_closed = app_closed || *(const uint8_t *)m.body;
            app_quit();
            break;
        default:
            break;
        }
}

void app_input(const struct sapp_event *e)
{
    /* What the machine asked of the window is answered first, and what the
     * event asks of the OS in return, the pointer's capture, is done after. */
    app_serve();
#ifndef __EMSCRIPTEN__
    if (prompt_event(e))
        return;
#endif
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_clipboard_read(e);
#endif
    switch (e->type)
    {
    case SAPP_EVENTTYPE_ICONIFIED:
    case SAPP_EVENTTYPE_RESTORED:
        app_flag(EVQ_MINIMIZED, e->type == SAPP_EVENTTYPE_ICONIFIED);
        break;
#ifndef __EMSCRIPTEN__
    /* The desktop window's focus. In the browser sokol's focus is the
     * browser window's, which several players on a page share, so the web
     * player's comes from its page instead (entry.c). */
    case SAPP_EVENTTYPE_FOCUSED:
    case SAPP_EVENTTYPE_UNFOCUSED:
        app_flag(EVQ_FOCUS, e->type == SAPP_EVENTTYPE_FOCUSED);
        break;
#endif
    case SAPP_EVENTTYPE_FILES_DROPPED:
        host_window_files_dropped();
        return;
    case SAPP_EVENTTYPE_QUIT_REQUESTED:
        /* A close the window did not ask for itself is the machine's to
         * answer: a chord the key behind it acted on (Alt-F4 on Windows)
         * keeps the window, and otherwise the machine says to close. */
        if (!app_quit_asked)
        {
            sapp_cancel_quit();
            evq_to_machine(EVQ_CLOSE, NULL, 0);
        }
        return;
    default:
        break;
    }
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
    {
        run_lock();
        const bool taken = dbgui_handle_event(e);
        run_unlock();
        if (taken)
            return;
    }
#endif
    input_event(e);
    input_apply();
}

int app_exit_code(void)
{
    /* A console break leaves through app_break_leave, which does not return, so
     * a break recorded here is the window closed on a running machine. */
    if (app_broke)
        return 1;
    return (app.exit_on_halt && proc_exited()) ? proc_get_exit_code() : 0;
}

void app_cleanup(void)
{
    /* The machine stops first, so nothing below runs beside it. proc_shutdown
     * runs before the sound, gfx and sg shut down, so every driver's stop hook
     * still has the host under it. Reaching here without app_quit_asked means
     * the close button stopped a machine that was still running, which is a
     * break. */
    run_stop();
    app_serve();
    machine_set_window(false);
    if (!app_quit_asked || app_closed)
        app_broke = true;
#ifndef __EMSCRIPTEN__
    /* The web player's machine is the worker's, which stops on its own. */
    proc_shutdown();
#endif
    /* app_break_leave does not return, so buffered output is flushed before
     * it. */
    if (app_broke && app_break_leave && app_break_asked && app_break_asked())
    {
        fflush(NULL);
        app_break_leave();
    }
#ifdef RP6502_PAD_HOST
    gamepad_input_stop();
#endif
#ifndef __EMSCRIPTEN__
    if (aud_enabled())
        host_audio_close();
#endif
#ifdef EMU_WITH_DEBUGGER
    dap_stop();
    if (dbg_is_active())
        dbgui_discard();
#endif
    gfx_shutdown();
    sg_shutdown();
}

void app_prepare(uint32_t *fb, double scale, bool have_scale, bool exit_on_halt,
                 int *out_w, int *out_h)
{
    /* Written as !(scale >= 0.1) so that a NaN also lands on the floor. */
    if (!(scale >= 0.1))
        scale = 0.1;
    if (scale > 64.0)
        scale = 64.0;
    app.exit_on_halt = exit_on_halt;
    evq_reset();
    input_reset();
    run_prepare(fb, exit_on_halt);
    bool fresh;
    gfx_set_frame(run_frame(&fresh));
    gfx_prepare(scale, have_scale, out_w, out_h);
}

#ifndef __EMSCRIPTEN__
/* Every desktop's window. high_dpi asks for a backbuffer at the display's own
 * resolution, where without it the compositor stretches one at the logical
 * size and smears the canvas; X11 has no such scaling, and sokol ignores it
 * there. The web player's is in its entry.c. */
int entry_run(uint32_t *fb, double scale, bool have_scale, bool exit_on_halt)
{
    int win_w, win_h;
    app_prepare(fb, scale, have_scale, exit_on_halt, &win_w, &win_h);
    sapp_run(&(sapp_desc){
        .init_cb = app_init,
        .frame_cb = app_frame,
        .event_cb = app_input,
        .cleanup_cb = app_cleanup,
        .width = win_w,
        .height = win_h,
        .high_dpi = true,
        .swap_interval = 1,
        .window_title = "Picocomputer 6502",
        .enable_dragndrop = true,
        .enable_clipboard = true,
        .clipboard_size = 65536,
        .logger.func = app_log,
#ifdef SOKOL_GLES3
        /* GLES 3.0 is all the drawing here needs. On Linux sokol would ask
         * for 3.1, so asking for less lets older GPUs open the window too,
         * and a driver still gives its newest compatible context. The web's
         * WebGL2 is 3.0 either way. */
        .gl = {.major_version = 3, .minor_version = 0},
#endif
    });
    return app_exit_code();
}
#endif

void app_log(const char *tag, uint32_t log_level, uint32_t log_item_id,
             const char *message_or_null, uint32_t line_nr,
             const char *filename_or_null, void *user_data)
{
    (void)user_data;
#ifndef __EMSCRIPTEN__
    if (log_item_id == SAPP_LOGITEM_CLIPBOARD_STRING_TOO_BIG && !strcmp(tag, "sapp"))
        app_clipboard_too_big = true;
#endif
    const char *message = message_or_null ? message_or_null : "";
    const char *filename = filename_or_null ? filename_or_null : "";
    switch (log_level)
    {
    case 0:
        RP6502_LOG(sokol, ERROR, "%s [%u] %s (%s:%u)", tag, log_item_id, message, filename, line_nr);
        abort();
    case 1:
        RP6502_LOG(sokol, ERROR, "%s [%u] %s (%s:%u)", tag, log_item_id, message, filename, line_nr);
        break;
    case 2:
        RP6502_LOG(sokol, WARN, "%s [%u] %s (%s:%u)", tag, log_item_id, message, filename, line_nr);
        break;
    default:
        RP6502_LOG(sokol, INFO, "%s [%u] %s (%s:%u)", tag, log_item_id, message, filename, line_nr);
        break;
    }
}
