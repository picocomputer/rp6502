/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Every host/sokol/<os>, and src/host/itch.io for the web, implements the
 * window functions here and nothing else implements any of them. A platform
 * with nothing to say for one of these writes an empty body. The host_gamepad_
 * three at the bottom are the desktops' alone, because app/gamepad.c, which is
 * what calls them, is built only for the desktop emulator.
 */

#ifndef _HOST_SOKOL_APP_ENTRY_H_
#define _HOST_SOKOL_APP_ENTRY_H_

#include <stdbool.h>
#include <stdint.h>

/* Open a sokol window and run the machine until it closes. sys_init has already
 * been called; a ROM has not necessarily been booted, because the window that
 * entry_wait_for_rom asks for opens with nothing loaded. fb is the caller-owned
 * framebuffer that vga renders into and the window presents, and it must hold
 * the largest canvas. scale may be fractional, and have_scale marks an explicit
 * --scale, which beats the remembered debug-session window size. exit_on_halt
 * closes the window when the program exits instead of leaving its final output
 * up. Returns app_exit_code. Android has no entry_run, because there
 * NativeActivity owns the entry and sokol_main stands in for main. */
int entry_run(uint32_t *fb, double scale, bool have_scale, bool exit_on_halt);

/* No ROM was supplied. A platform that can still receive one by drag and drop
 * arms its on-screen prompt and returns true so the caller opens the window.
 * One that cannot, such as the web page that is itself one program, returns
 * false and the caller prints usage and exits. */
bool entry_wait_for_rom(void);

/* Resize the OS window to w x h framebuffer pixels. X11 and Win32 only. */
void host_window_resize(int w, int h);

/* Ask the window manager to keep the canvas aspect cw:ch during an interactive
 * resize. X11 only. */
void host_window_set_aspect_hint(int cw, int ch);

/* Per-platform setup, from the sokol init callback. */
void host_window_init(void);

/* True while a platform-owned overlay is up: the Android ROM menu, or a
 * desktop's drop-a-ROM prompt. The canvas is not drawn while it is, and a
 * halted program is not treated as a program exiting. */
bool host_window_menu_active(void);

/* Draw that overlay into the current swapchain pass. */
void host_window_menu_draw(void);

/* A file was dropped on the window. Desktop platforms pass the path to
 * app_boot_rom; web and Android do not enable drag and drop, so nothing calls
 * this there. */
void host_window_files_dropped(void);

/* Open a URL in the user's default browser, for the docs link under the
 * drop-a-ROM prompt. Desktop only. */
void host_window_open_url(const char *url);

/* One host controller, in the units gamepad_host_report takes, because the
 * scaling belongs where the ranges are known. A backend sets type only when it
 * is certain of the face-button labels, and sets sticks only when it found
 * both. */
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
 * gamepad block mapped and not before, because the emulator must not open an
 * input device until a program asks for one. False when the host has nothing to
 * offer, which is ordinary and is retried. */
bool host_gamepad_open(void);

void host_gamepad_close(void);

/* What is connected now, newest state, up to max entries. Returns the count.
 * Called once per presented frame while a program has the block mapped. */
int host_gamepad_poll(gamepad_host_t *gamepads, int max);

#endif /* _HOST_SOKOL_APP_ENTRY_H_ */
