/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Internal interface between the shared, host-neutral window core
 * (app/window_core.c) and the per-host window files (host/<os>/window.c).
 * Not a public header — app code uses app/window.h. The per-host file supplies
 * the entry point (window_run / sokol_main) and the host_window_* hooks; the
 * core supplies the render/frame/present pipeline and the four sokol callbacks.
 */

#ifndef _EMU_APP_WINDOW_CORE_H_
#define _EMU_APP_WINDOW_CORE_H_

#include <stdbool.h>
#include <stdint.h>
#include "sokol/sokol_app.h"

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

/* The process exit code once sokol returns: the ROM's exit code when it halted
 * the app outside debug mode, else 0. The per-host window_run returns this. */
int window_core_exit_code(void);

/* Boot a .rp6502 (rom_load + cold boot + fresh argv), true on success. The path
 * is host UTF-8; conversion to the guest's OEM code page happens here, so hosts
 * pass what the OS handed them (a lossy spelling never boots — pre-substitute
 * one that converts, like the Windows 8.3 fallback). Ignored while a debug
 * session owns the machine. A failed load halts the machine: rom_load streams
 * into live RAM before it can fail, so the old program may already be
 * clobbered — matching hardware, where a failed LOAD leaves the CPU stopped in
 * the monitor. */
bool window_core_boot_rom(const char *path);

/* Stand up the text + vector renderers for the desktop no-ROM prompt. Call once
 * from the sokol init callback (host_window_init, after sg_setup). Only the
 * desktop hosts call it, so web/Android link none of it. */
void window_core_prompt_setup(void);

/* Draw the centered "drop a ROM" card (dark rounded box, heavy dashed border, the
 * two message lines in the border color) into the current swapchain pass. Pairs
 * with a host's host_window_menu_active() being true, which suppresses the canvas. */
void window_core_draw_prompt(const char *line1, const char *line2);

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

/* Open a URL in the user's default browser (desktop hosts; no-op on web/Android).
 * Called when the docs link under the drop-a-ROM prompt is clicked. */
void host_window_open_url(const char *url);

#endif /* _EMU_APP_WINDOW_CORE_H_ */
