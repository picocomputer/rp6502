/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Web (Emscripten) window host: the sokol entry (window_run -> sapp_run, which
 * runs the browser main loop). The canvas is managed by the page, so resize and
 * aspect hints are no-ops; the render/frame/present pipeline is in
 * host/window_core.c.
 */

#include "emu/host/window.h"
#include "emu/host/window_core.h"
#include "sokol_app.h"
#include "sokol_log.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void host_window_resize(int w, int h) { (void)w, (void)h; }
void host_window_set_aspect_hint(int cw, int ch) { (void)cw, (void)ch; }
void host_window_init(void) {}
bool host_window_menu_active(void) { return false; }
void host_window_menu_draw(void) {}

/* HTML5 drops carry content, not paths: stage the fetched bytes in MEMFS /tmp
 * (never the persistent IDBFS mount) so the shared boot path can rom_load it.
 * Each drop gets its own staging file — the running program reads its ROM:
 * assets (and a self-exec's argv[0]) from its file on demand, so it must
 * survive until a NEW program has booted from a different one. */
static char drop_cur[32]; /* the running drop-boot's staging file, "" = none */

static void drop_fetched(const sapp_html5_fetch_response *r)
{
    static int drop_seq;
    if (r->succeeded)
    {
        char next[32];
        snprintf(next, sizeof next, "/tmp/dropped-%d.rp6502", ++drop_seq);
        FILE *f = fopen(next, "wb");
        if (f)
        {
            size_t put = fwrite(r->data.ptr, 1, r->data.size, f);
            fclose(f);
            if (put == r->data.size && window_core_boot_rom(next))
            {
                if (drop_cur[0])
                    remove(drop_cur);
                snprintf(drop_cur, sizeof drop_cur, "%s", next);
            }
            else
                remove(next);
        }
    }
    free((void *)r->buffer.ptr);
}

void host_window_files_dropped(void)
{
    uint32_t size = sapp_html5_get_dropped_file_size(0);
    void *buf = malloc(size ? size : 1); /* zero-size drop still needs a live ptr */
    if (!buf)
        return;
    sapp_html5_fetch_dropped_file(&(sapp_html5_fetch_request){
        .dropped_file_index = 0,
        .callback = drop_fetched,
        .buffer = {.ptr = buf, .size = size ? size : 1},
    });
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
        .enable_clipboard = true, /* browser paste types into the emulated keyboard */
        .clipboard_size = 65536,
        .logger.func = slog_func,
    });
    return 0;
}
