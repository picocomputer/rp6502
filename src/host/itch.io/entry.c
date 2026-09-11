/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/prompt.h"
#include "sokol/sokol_app.h"
#include <stdint.h>

void host_window_resize(int w, int h) { (void)w, (void)h; }
void host_window_set_aspect_hint(int cw, int ch) { (void)cw, (void)ch; }
void host_window_init(void) {}
bool host_window_menu_active(void) { return false; }
void host_window_menu_draw(void) {}
void host_window_files_dropped(void) {}
void host_window_open_url(const char *url) { (void)url; }
bool entry_wait_for_rom(void) { return false; }

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
        .swap_interval = 1,
        .window_title = "Picocomputer 6502",
        .enable_clipboard = true,
        .clipboard_size = 65536,
        .logger.func = app_log,
    });
    return app_exit_code();
}
