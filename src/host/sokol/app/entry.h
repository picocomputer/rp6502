/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What a platform directory answers. Every host/sokol/<os> (and the web root
 * beside it) implements this file and nothing else implements any of it: the
 * entry, because sokol's desc and entry convention differ per OS, and the hooks
 * the shared application calls when it needs something only that OS can do.
 *
 * The application never asks which OS it is on — there is not one platform
 * #ifdef in app.c. When it needs something platform-shaped it gains a hook
 * here; where a platform has nothing to say it writes an empty body.
 */

#ifndef _HOST_SOKOL_APP_ENTRY_H_
#define _HOST_SOKOL_APP_ENTRY_H_

#include <stdbool.h>
#include <stdint.h>

/* Open a sokol window and run the machine until closed. The ROM must already be
 * loaded and sys_init() called. fb is the caller-owned framebuffer (must hold
 * the largest canvas); vga renders into it and the window presents it. scale
 * may be fractional; have_scale marks an explicit --scale, which beats the
 * remembered debug-session window size. The title shows "(stopped)" once the
 * program exits; exit_on_halt closes the window then instead of leaving the
 * final output up. Returns app_exit_code(). Android has no entry_run: there
 * NativeActivity owns the entry and sokol_main stands in for main(). */
int entry_run(uint32_t *fb, double scale, bool have_scale, bool exit_on_halt);

/* No ROM was supplied. A platform that can still receive one (desktop
 * drag-and-drop) arms its on-screen "drop a .rp6502 here" prompt, holds the
 * machine, and returns true so the caller opens the window. One that cannot
 * (web: the page is one program; headless: no window) returns false, and the
 * caller prints usage and exits. */
bool entry_wait_for_rom(void);

/* ---- what the application calls when only the OS can answer ---- */

/* Resize the OS window to w x h framebuffer px (X11/Win32; no-op elsewhere). */
void host_window_resize(int w, int h);

/* Ask the WM to keep the canvas aspect cw:ch during interactive resizes (X11;
 * no-op elsewhere). */
void host_window_set_aspect_hint(int cw, int ch);

/* Per-platform setup inside the sokol init callback (Android stands up its text
 * overlay; no-op elsewhere). */
void host_window_init(void);

/* True while a platform-owned modal overlay is up (the Android ROM menu): the
 * application freezes emulation and shows the overlay instead of the canvas.
 * Always false everywhere else. */
bool host_window_menu_active(void);

/* Draw the platform-owned overlay into the current swapchain pass (the Android
 * ROM menu; no-op elsewhere). */
void host_window_menu_draw(void);

/* A file was dropped on the window: boot it. Desktop platforms pass the dropped
 * path to app_boot_rom; web and Android don't enable drag-n-drop, so the hook
 * never fires there. */
void host_window_files_dropped(void);

/* Open a URL in the user's default browser (desktop; no-op on web/Android).
 * Called when the docs link under the drop-a-ROM prompt is clicked. */
void host_window_open_url(const char *url);

/* One host controller, in the units gamepad_host_report takes, because scaling
 * belongs where the ranges are known. A backend claims a type only when it is
 * certain of the labels, and sticks only when it found both. */
typedef struct
{
    uint64_t id; /* stable while plugged, so a player keeps its number */
    uint8_t dpad, button0, button1;
    int8_t lx, ly, rx, ry;
    uint8_t lt, rt;
    uint8_t type; /* GAMEPAD_TYPE_ */
    bool sticks;
} gamepad_host_t;

/* Start reading controllers. Called on the first frame a program has the
 * gamepad block mapped, and not before — until then the emulator must not
 * touch an input device. False when the host has nothing to offer, which is
 * ordinary and is retried. */
bool host_gamepad_open(void);

/* Stop reading, release everything, and expect host_gamepad_open again. */
void host_gamepad_close(void);

/* What is connected now, newest state, up to max entries. Returns the count.
 * Called once per presented frame while a program has the block mapped. */
int host_gamepad_poll(gamepad_host_t *gamepads, int max);

#endif /* _HOST_SOKOL_APP_ENTRY_H_ */
