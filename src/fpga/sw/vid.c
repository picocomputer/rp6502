/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The frame publish: term.c owns the scroll remap and the cursor; once per
 * frame this snapshots the resolved scanout state into the vid register
 * shadows, which the raster latches at the next frame start. One frame of
 * latency, never a tear.
 */

#include "mmio.h"
#include "vid.h"

#include "vga/term/term.h"

void vid_task(void)
{
    static uint32_t frame;
    uint32_t now = VID_FRAME;
    if (now == frame)
        return;
    frame = now;
    term_scanout_t so;
    term_scanout(&so);
    for (uint32_t y = 0; y < so.height; y++)
        VID_ROW(y) = (uint32_t)so.row[y] & 0xFFFF;
    VID_CURSOR = ((uint32_t)so.cursor_enabled << 25)
                 | ((uint32_t)so.cursor_lit << 24)
                 | ((uint32_t)so.cursor_style << 16)
                 | ((uint32_t)so.cursor_y << 8) | so.cursor_x;
    VID_CURSOR_COLOR = so.cursor_color;
    VID_BLINK = so.blink_phase;
}
