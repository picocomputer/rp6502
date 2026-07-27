/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's terminal view: mode0.c's role, over scanout hardware.
 * term.c owns the model; vid_init programs the raster window and the
 * terminal geometry, and once per frame vid_task snapshots the visible
 * terminal — resolved row bases, cursor, blink — into the vid register
 * shadows the raster latches at the next frame start. One frame of
 * latency, never a tear.
 */

#include "mmio.h"
#include "vid.h"

#include "vga/term/term.h"

#include <stdint.h>

void vid_init(void)
{
    term_set_height(80, 30);
    VID_PROG = 0x80000000u | ((uint32_t)480 << 16);
}

void vid_task(void)
{
    static uint32_t frame;
    uint32_t now = VID_FRAME;
    if (now == frame)
        return;
    frame = now;
    term_view_t tv;
    term_view(&tv);
    for (uint32_t y = 0; y < tv.height; y++)
        VID_ROW(y) = (uint32_t)term_view_row(y) & 0xFFFF;
    VID_CURSOR = ((uint32_t)tv.cursor_enabled << 25)
                 | ((uint32_t)tv.cursor_lit << 24)
                 | ((uint32_t)tv.cursor_style << 16)
                 | ((uint32_t)tv.cursor_y << 8) | tv.cursor_x;
    VID_CURSOR_COLOR = tv.cursor_color;
    VID_BLINK = tv.blink_phase;
}
