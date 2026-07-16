/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * macOS window host: the sokol entry (window_run -> sapp_run). Cocoa needs no
 * manual resize or aspect hint, so those hooks are no-ops; the render/frame/
 * present pipeline is in host/window_core.c.
 */

#include "emu/host/window.h"
#include "emu/host/window_core.h"
#include "sokol_app.h"
#include "sokol_log.h"
#include <stdint.h>

void host_window_resize(int w, int h) { (void)w, (void)h; }
void host_window_set_aspect_hint(int cw, int ch) { (void)cw, (void)ch; }
void host_window_init(void) {}
bool host_window_menu_active(void) { return false; }
void host_window_menu_draw(void) {}

void host_window_files_dropped(void)
{
    window_core_boot_rom(sapp_get_dropped_file_path(0));
}

int window_run(uint32_t *fb, double scale, bool have_scale, bool vsync, bool exit_on_halt)
{
    int win_w, win_h;
    window_core_prepare(fb, scale, have_scale, vsync, exit_on_halt, &win_w, &win_h);
    sapp_run(&(sapp_desc){
        .init_cb = window_core_init,
        .frame_cb = window_core_frame,
        .event_cb = window_core_event,
        .cleanup_cb = window_core_cleanup,
        .width = win_w,
        .height = win_h,
        .swap_interval = vsync ? 1 : 0, /* off: present uncapped (driver may ignore) */
        .window_title = "Picocomputer 6502",
        .enable_dragndrop = true, /* drop a .rp6502 to boot it */
        .enable_clipboard = true, /* Cmd+V types into the emulated keyboard */
        .clipboard_size = 65536,
        .logger.func = slog_func,
    });
    return 0;
}
