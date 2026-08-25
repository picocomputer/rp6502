/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Which canvas is up, and what happens when that changes.
 *
 * There are five, they are a fact about the ABI rather than about any
 * machine, and choosing one throws away everything programmed on the last:
 * a mode program describes scanlines of a particular size and means nothing
 * once the size is different. Returning to the console reinstalls the
 * terminal, so a program that hands the machine back leaves a screen that
 * still draws.
 *
 * What a machine supplies is how it forgets -- a table to sweep or registers
 * to blank -- and whether anything has to be told which canvas is up.
 */

#include "core/vga/vga.h"
#include "core/vga/mode0.h"

static vga_canvas_t canvas_code = vga_canvas_console;
static int16_t canvas_w = 640, canvas_h = 480;

bool vga_canvas_select(uint16_t canvas)
{
    switch (canvas)
    {
    case vga_canvas_console:
    case vga_canvas_320_240:
    case vga_canvas_320_180:
    case vga_canvas_640_480:
    case vga_canvas_640_360:
        break;
    default:
        return false; /* no such canvas: nothing changes, and the write NAKs */
    }
    canvas_code = (vga_canvas_t)canvas;
    int w, h;
    vga_canvas_geometry(canvas_code, &w, &h);
    canvas_w = (int16_t)w;
    canvas_h = (int16_t)h;
    vga_canvas_reset();
    vga_canvas_publish(canvas_code);
    if (canvas_code == vga_canvas_console)
    {
        uint16_t xregs[8] = {0};
        mode0_prog(xregs); /* the console term, across the whole canvas */
    }
    return true;
}

vga_canvas_t vga_get_canvas(void)
{
    return canvas_code;
}

bool vga_canvas_is_console(void)
{
    return canvas_code == vga_canvas_console;
}

/* Kept rather than derived: every plane booked against a mode program asks
 * for it, which is often enough to matter on the machine with no cache. */
int16_t vga_canvas_height(void)
{
    return canvas_h;
}

int16_t vga_canvas_width(void)
{
    return canvas_w;
}

void vga_canvas_size(int *w, int *h)
{
    *w = canvas_w;
    *h = canvas_h;
}
