/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's terminal view: mode0.c's role, over scanout hardware.
 * term.c owns the model; once per frame vid_task snapshots it into the
 * register shadows the raster latches at the next frame start. One frame
 * of latency, never a tear.
 */

#include "core/api/xreg.h"
#include "font.h"
#include "mmio.h"
#include "vga.h"
#include "vid.h"

#include "core/term/term.h"

#include <stdint.h>

/* The window the terminal draws in, kept because the register it goes
 * to is written once here and read by nothing: after a wake there is
 * no way to ask the fabric what it used to be. */
static uint32_t vid_prog_word;

bool mode0_prog(uint16_t *xregs)
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
    if (!vga_prog_exclusive(plane, scanline_begin, scanline_end, 0, NULL))
        return false;
    if (use_40)
        term_set_height(40, (uint8_t)(scanline_count / 8));
    else
        term_set_height(80, (uint8_t)(scanline_count / 16));
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

/* Set by vid_stop, performed by vid_task. It rides the savestate blob like
 * everything else in here, which is right: a machine snapped between the two
 * wakes still owing itself the restore. */
static bool vid_needs_restore;

void vid_task(void)
{
    /* Ahead of the frame gate on purpose. A stop commits at the end of a
     * pass, so this is the first thing that runs afterwards, and a relaunch
     * cannot be committed before the next pass -- which means the restore
     * always lands before the next program's RESB goes up. Behind the gate it
     * could slip a frame and arrive after the program had started drawing. */
    if (vid_needs_restore)
    {
        vid_needs_restore = false;
        xreg1(0x0F, 0x01, 437);
        xreg1(0x0F, 0x00, vga_get_display_type());
    }
    static uint32_t frame;
    uint32_t now = VID_FRAME;
    if (now == frame)
        return;
    frame = now;
    vid_publish();
}

/* The fabric's own count, which is the display's and not this firmware's --
 * it advances whether or not the task above ran. */
unsigned long vga_frame_count(void)
{
    return VID_FRAME;
}

/* The row table and the cursor would come back on their own at the
 * next frame, so this is only the window -- and then the view, so the
 * frame in between is not a screenful of row zero. */
/* For the wake log: the window nothing can read back out of the
 * fabric. */
uint32_t vid_prog_word_get(void)
{
    return vid_prog_word;
}

void vid_restore(void)
{
    VID_PROG = vid_prog_word;
    vid_publish();
}

/* The console's code page and display type, restored on the way out of a
 * program -- deferred to vid_task, which is where the RIA's vga_task puts the
 * same two writes behind the same kind of flag. Deferring is what frees this
 * driver's position in the list: the restore has to follow everything that could
 * still draw, and a stop hook can only promise that by being last. A task can
 * promise it from anywhere, because it runs after the whole fan-out. */
void vid_stop(void)
{
    vid_needs_restore = true;
}
