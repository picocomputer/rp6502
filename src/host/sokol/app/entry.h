/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Every host/sokol/<os>, and src/host/web for the web, implements the
 * window functions here. A platform that has nothing to do for one of these
 * implements it with an empty body. The desktops share three things: entry_run
 * (app/app.c), and the drop-a-ROM prompt's entry_wait_for_rom,
 * host_window_menu_ functions and host_window_files_dropped (app/prompt.c);
 * the web has its own of each in its entry.c. Only Windows implements
 * entry_argv_utf8, and only the web entry_canvas_locked.
 * Only the desktop builds implement host_window_resize, because the debugger's
 * Window Scale menu, which asks for it (gfx_canvas_changed carries it out), is
 * built only for the desktop emulator. Every platform implements the three
 * host_gamepad_ functions at the bottom, the web from the page's Gamepad API,
 * and app/gamepad.c calls them.
 */

#ifndef _HOST_SOKOL_APP_ENTRY_H_
#define _HOST_SOKOL_APP_ENTRY_H_

#include <stdbool.h>
#include <stdint.h>
#include "core/sys/event.h"

/* Open a sokol window and run the machine until it closes. sys_init has already
 * been called; a ROM has not necessarily been booted, because the window opens
 * with nothing loaded in two cases: under --dap, and after entry_wait_for_rom
 * returns true. fb is the caller-owned framebuffer that vga renders into and
 * the window presents, the first of the frames a machine on a thread of its
 * own draws into (run.h), and it must hold the largest canvas. scale may be
 * fractional, and have_scale marks an explicit --scale, which beats the saved
 * debug-session window size. exit_on_halt closes the window when the program
 * exits instead of leaving its final output up. Returns app_exit_code. */
int entry_run(uint32_t *fb, double scale, bool have_scale, bool exit_on_halt);

/* No ROM was supplied. A platform that can still receive one by drag and drop
 * arms its on-screen prompt and returns true so the caller opens the window.
 * One that cannot, such as the web page that is itself one program, returns
 * false and the caller prints usage and exits. */
bool entry_wait_for_rom(void);

/* Resize the OS window's client area to w x h framebuffer pixels, keeping its
 * top-left corner where it is. A maximized or fullscreen window keeps the size
 * the window manager gave it. Desktop only. */
void host_window_resize(int w, int h);

/* Keep the window at the frame aspect w:h, which on the web is the player box.
 * A desktop holds its client area at w:h while the user drags an edge, and with
 * refit it also reshapes the window now, through host_window_resize, keeping
 * the width and changing the height. The web gives the box that shape and
 * ignores refit, because the page sizes the box. */
void host_window_set_aspect(int w, int h, bool refit);

/* Per-platform setup, from the sokol init callback. */
void host_window_init(void);

/* True while a platform overlay is up, such as a desktop's drop-a-ROM prompt.
 * The canvas is not drawn while it is, and a halted program is not treated as
 * a program exiting, which the machine learns from EVQ_MENU and keeps its
 * own mirror of (machine_menu_up). */
bool host_window_menu_active(void);

/* Draw that overlay into the current swapchain pass. */
void host_window_menu_draw(void);

/* A file was dropped on the window. Desktop platforms send its path to the
 * machine (EVQ_BOOT), which boots it; the web does not enable drag and
 * drop, so nothing calls this there. */
void host_window_files_dropped(void);

/* Open a URL in the user's default browser, for the docs link under the
 * drop-a-ROM prompt. Desktop only. */
void host_window_open_url(const char *url);

/* NumLock, Caps Lock and Scroll Lock as the system has them now, in
 * KEYBOARD_LED_ bits. The system owns the locks and their lights, so a window
 * reads them and never changes one. A lock the system does not have reads the
 * way its keyboard always acts. */
uint8_t host_locks(void);

/* The argv of main() on Windows is in the ANSI code page, which cannot hold
 * every path, so the command line is read again as UTF-16 and returned in
 * UTF-8, the encoding of argv on every other host. NULL if it cannot be read.
 * The result lasts for the whole run. */
char **entry_argv_utf8(int *argc);

/* Writes the browser's clipboard, on the web. */
void entry_clipboard_write(const char *utf8);

/* True while the pointer lock is on Module.canvas. On the web,
 * sapp_mouse_locked() is true while the lock is on any canvas in the page. */
bool entry_canvas_locked(void);

/* The sound, on a desktop: a device that asks for it on a thread of its own.
 * render fills dst with frames of stereo float at the device's rate, which
 * is given each time, since it changes when the default device does. Opened
 * from the init callback and closed from cleanup. False when there is no
 * sound to be had. Each desktop's driver is its audio.c: WASAPI on Windows,
 * PipeWire's own API on Linux where the Pulse server is PipeWire and
 * PulseAudio otherwise, the default output AudioUnit on macOS. */
bool host_audio_open(void (*render)(float *dst, int frames, uint32_t rate));
void host_audio_close(void);


/* Start reading controllers. Called on the first frame a program has the
 * gamepad block mapped and not before, because the emulator must not open an
 * input device until a program asks for one. False when the host has nothing to
 * offer, which is ordinary and is retried. */
bool host_gamepad_open(void);

void host_gamepad_close(void);

/* What is connected now, newest state, up to max entries. Returns the count.
 * Called once per presented frame while a program has the block mapped. */
int host_gamepad_poll(gamepad_host_t *gamepads, int max);

#endif /* _HOST_SOKOL_APP_ENTRY_H_ */
