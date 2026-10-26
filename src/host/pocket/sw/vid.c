/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * vid_task copies term.c's view into the mode 0 register shadows at most once
 * per frame. mode0.sv latches the shadows one line before each frame starts,
 * so the row table, cursor and blink phase that the scanout uses change only
 * between frames.
 */

#include "core/api/xreg.h"
#include "font.h"
#include "mmio.h"
#include "vga.h"
#include "vid.h"

#include "core/term/term.h"
#include "core/vga/mode/mode0.h"

#include <stdint.h>

/* The blob does not contain VID_PROG, so the value written to it is kept
 * here for vid_restore. */
static uint32_t vid_prog_word;

bool mode0_prog(uint16_t *xregs)
{
    int16_t plane = (int16_t)xregs[2];
    int16_t scanline_begin = (int16_t)xregs[3];
    int16_t scanline_end = (int16_t)xregs[4];
    uint8_t cols, rows;
    if (!mode0_layout(vga_canvas_width(), vga_canvas_height(),
                      &scanline_begin, &scanline_end, &cols, &rows))
        return false;
    if (!vga_prog_exclusive(plane, scanline_begin, scanline_end, 0, NULL))
        return false;
    term_set_height(cols, rows);
    vid_prog_word = 0x80000000u | ((uint32_t)(uint16_t)scanline_end << 16)
                    | (uint16_t)scanline_begin;
    VID_PROG = vid_prog_word;
    return true;
}

void vid_init(void)
{
    font_init();
    vga_canvas_select(0);
}

static void vid_publish(void)
{
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

static bool vid_needs_restore;

void vid_task(void)
{
    if (vid_needs_restore)
    {
        vid_needs_restore = false;
        xreg1(0x0F, 0x01, 437);
        vga_console_reset();
    }
    static uint32_t frame;
    uint32_t now = VID_FRAME;
    if (now == frame)
        return;
    frame = now;
    vid_publish();
}

unsigned long vga_frame_count(void)
{
    return VID_FRAME;
}

uint32_t vid_prog_word_get(void)
{
    return vid_prog_word;
}

void vid_restore(void)
{
    VID_PROG = vid_prog_word;
    vid_publish();
}

/* The console's code page and canvas are put back when a program stops.
 * Both are deferred to vid_task so that they land after every other stop hook
 * has run, which a stop hook itself could ensure only from the first row of
 * the driver list, because stop hooks run in reverse order. */
void vid_stop(void)
{
    vid_needs_restore = true;
}
