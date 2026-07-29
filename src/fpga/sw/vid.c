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
#include "vga.h"
#include "vid.h"

#include "vga/term/font.h"
#include "vga/term/term.h"

#include <stdint.h>

/* mode0_prog over this machine's view: same defaults and validation, the
 * raster window landing in the scanout register instead of a prog entry.
 * Rendering the terminal on a graphics canvas is deferred; the ACK still
 * matches the oracle's. */
bool vid_mode0_prog(uint16_t *xregs)
{
    int16_t plane = (int16_t)xregs[2];
    int16_t scanline_begin = (int16_t)xregs[3];
    int16_t scanline_end = (int16_t)xregs[4];
    int16_t height = vga_canvas_height();
    if (!scanline_begin && !scanline_end)
    {
        if (height == 180)
            scanline_begin = 2, scanline_end = 178;
        if (height == 360)
            scanline_begin = 4, scanline_end = 356;
    }
    if (!scanline_end)
        scanline_end = height;
    int16_t scanline_count = (int16_t)(scanline_end - scanline_begin);
    bool use_40 = height == 180 || height == 240;
    if (!scanline_count || scanline_count % (use_40 ? 8 : 16))
        return false;
    /* vga_prog_valid's checks, in the oracle's order. */
    if (plane < 0 || plane >= 3 || scanline_begin < 0 ||
        scanline_end > height || scanline_count < 1)
        return false;
    if (use_40)
        term_set_height(40, (uint8_t)(scanline_count / 8));
    else
        term_set_height(80, (uint8_t)(scanline_count / 16));
    VID_PROG = 0x80000000u | ((uint32_t)(uint16_t)scanline_end << 16)
               | (uint16_t)scanline_begin;
    return true;
}

/* The glyphs are the video device's memory here, not a table built into
 * the fabric, so the firmware stores them the way font.c builds them. A
 * code page rewrites only the high half of each row, but a whole face is
 * fifteen hundred words and this runs once per change. Stores are whole
 * words because byte lanes are what stop the fabric inferring a block
 * RAM at all. */
static void vid_font_store(void)
{
    const uint32_t *src16 = (const uint32_t *)(const void *)font16;
    const uint32_t *src8 = (const uint32_t *)(const void *)font8;
    for (unsigned i = 0; i < sizeof(font16) / 4; i++)
        VID_FONT16[i] = src16[i];
    for (unsigned i = 0; i < sizeof(font8) / 4; i++)
        VID_FONT8[i] = src8[i];
}

void vid_set_code_page(uint16_t cp)
{
    font_set_code_page(cp);
    vid_font_store();
}

uint16_t vid_get_code_page(void)
{
    return font_get_code_page();
}

void vid_init(void)
{
    font_init();
    vid_font_store();
    vga_set_canvas(0);
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
