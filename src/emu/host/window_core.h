/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Internal interface between the shared, host-neutral window core
 * (host/window_core.c) and the per-host window files (host/<os>/window.c).
 * Not a public header — app code uses host/window.h. The per-host file supplies
 * the entry point (window_run / sokol_main) and the host_window_* hooks; the
 * core supplies the render/frame/present pipeline and the four sokol callbacks.
 */

#ifndef _EMU_HOST_WINDOW_CORE_H_
#define _EMU_HOST_WINDOW_CORE_H_

#include <stdbool.h>
#include <stdint.h>
#include "sokol_app.h"

/* Seed the core's window/render state from the launch options and report the
 * window's initial pixel size (canvas aspect at the requested scale, plus the
 * debugger menu strip). The per-host entry calls this before starting sokol,
 * then builds a sapp_desc around the four window_core_* callbacks below. */
void window_core_prepare(uint32_t *fb, double scale, bool have_scale, bool vsync,
                         bool exit_on_halt, int *out_w, int *out_h);

/* The four sokol lifecycle callbacks each per-host sapp_desc points at. */
void window_core_init(void);
void window_core_frame(void);
void window_core_event(const sapp_event *e);
void window_core_cleanup(void);

/* Boot a .rp6502 (rom_load + cold boot + fresh argv), true on success. Ignored
 * while a debug session owns the machine. A failed load halts the machine:
 * rom_load streams into live RAM before it can fail, so the old program may
 * already be clobbered — matching hardware, where a failed LOAD leaves the CPU
 * stopped in the monitor. */
bool window_core_boot_rom(const char *path);

/* ---- hooks the core calls, implemented per host ---- */

/* Resize the OS window to w x h framebuffer px (X11/Win32; no-op elsewhere). */
void host_window_resize(int w, int h);

/* Ask the WM to keep the canvas aspect cw:ch during interactive resizes (X11;
 * no-op elsewhere). */
void host_window_set_aspect_hint(int cw, int ch);

/* Per-host setup inside the sokol init callback (Android stands up its text
 * overlay; no-op elsewhere). */
void host_window_init(void);

/* True while a host-owned modal overlay is up (the Android ROM menu): the core
 * freezes emulation and shows the overlay instead of the canvas. Always false on
 * every other host. */
bool host_window_menu_active(void);

/* Draw the host-owned overlay into the current swapchain pass (the Android ROM
 * menu; no-op elsewhere). */
void host_window_menu_draw(void);

/* A file was dropped on the window: boot it. Desktop hosts pass the dropped
 * path to window_core_boot_rom; web and Android don't enable drag-n-drop, so
 * the hook never fires there. */
void host_window_files_dropped(void);

#endif /* _EMU_HOST_WINDOW_CORE_H_ */
