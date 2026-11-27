/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What runs the machine for a window: the pace, the frames, and the finished
 * frames handed to the window.
 *
 * On the desktop the machine runs on a thread of its own, so nothing that
 * holds up the window holds up the machine and its sound: a present the
 * compositor keeps for a tenth of a second, a window being moved, a screen's
 * scale changing. The window tells the machine when each display callback
 * comes (run_display) and shows the newest frame the machine finished
 * (run_frame). Everything else between them is a message (seam.h), which the
 * machine takes between its steps and its frames. The web player's machine
 * runs in a worker and its window on the page (host/web/link.h).
 *
 * One lock is the machine's, for a debugger alone: its windows and its
 * adapter read and change the machine as it stands. The machine's thread
 * holds it while it runs, and lets a debugger waiting for it have it
 * between its steps and frames; with no debugger up nothing else takes it.
 * It is a lock the holder may take again, because a window's own calls into
 * the OS can deliver an event to it on the spot. Nothing holding it waits on
 * the window, and the window never holds it while it waits on the machine or
 * on the display.
 */

#ifndef _HOST_SOKOL_APP_RUN_H_
#define _HOST_SOKOL_APP_RUN_H_

#include <stdbool.h>
#include <stdint.h>

/* Makes the lock, before anything may take it: the debug adapter starts
 * before the window opens. run_prepare calls it too. */
void run_setup(void);

/* Before the window opens. fb is the caller's frame, large enough for the
 * largest canvas, which is the first of the frames the machine draws into.
 * exit_on_halt runs one more frame after the program exits, to be the last
 * picture, and then asks the window to close. */
void run_prepare(uint32_t *fb, bool exit_on_halt);

/* --phi2 0: frames back to back, as fast as the host runs them. */
void run_set_unpaced(bool on);

/* From the window's init, once everything the machine reaches is up, and
 * from its cleanup, before anything the machine reaches goes down. run_stop
 * keeps the lock to the end of the process, so the teardown runs alone. */
void run_start(void);
void run_stop(void);

/* The machine's lock, for the debugger's windows and its adapter. */
void run_lock(void);
void run_unlock(void);

/* Wakes the machine's thread to look at what it has been given: a debug
 * adapter request, say. */
void run_wake(void);

/* A display callback at now, dt after the one before, whose present waited
 * for the display or not. It owes the machine the time to now and the frames
 * it makes due. On the desktop it then waits, without the lock and for no
 * longer than room, for the machine to run them, so the frame the callback
 * presents is the one due at it, as it was when the callback ran the frames
 * itself. A present that does not wait gives the callback no frame of its
 * own, and it waits for the next new one. */
void run_display(uint64_t now, uint64_t dt, bool paced, uint64_t room);

/* A frame the machine finished, and the shape it was drawn in. */
typedef struct run_frame_t
{
    const uint32_t *pixels; /* canvas_w by canvas_h */
    int canvas_w, canvas_h;
    int console_w, console_h;
    bool last; /* exit_on_halt's last picture */
} run_frame_t;

/* The newest finished frame, which stays the window's until the next call.
 * fresh is whether it is one this call has not given before. From the
 * window. */
const run_frame_t *run_frame(bool *fresh);

/* Whether the frame run_frame gave is the last picture of a program that
 * exited under exit_on_halt, after which the window closes. */
bool run_quit_asked(void);

#ifdef __EMSCRIPTEN__
/* The web player's halves (host/web/link.h). The page's frame, which a frame
 * from the worker is copied into, and its shape once it is; the worker's
 * machine run for what is due now, giving how long until more is due; and
 * how far the page's clock is ahead of the worker's. */
uint32_t *run_web_frame_pixels(void);
void run_web_frame_done(int w, int h, int cw, int ch, bool last);
uint64_t run_web_step(void);
void run_web_clock(int64_t page_minus_worker_ns);
void run_web_up(void);
#endif

/* The host time spent running frames, for the debugger's pace window. */
uint64_t run_machine_ns(void);

#endif /* _HOST_SOKOL_APP_RUN_H_ */
