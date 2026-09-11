/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/vga.h"
#include "core/vga/mode/mode0.h"

static vga_canvas_t canvas_code = vga_canvas_console;
static int16_t canvas_w = 640, canvas_h = 480;

/* The savestate load restores the scanline table and the terminal's first line
 * after this call, so this function must not reset the table or re-program the
 * terminal the way vga_canvas_select does. */
bool vga_canvas_load(uint16_t canvas)
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
        return false;
    }
    int w, h;
    vga_canvas_geometry((vga_canvas_t)canvas, &w, &h);
    canvas_code = (vga_canvas_t)canvas;
    canvas_w = (int16_t)w;
    canvas_h = (int16_t)h;
    vga_canvas_publish(canvas_code);
    return true;
}

vga_canvas_t vga_canvas_code(void)
{
    return canvas_code;
}

/* Selecting a canvas discards everything programmed on the last, because a
 * mode program describes scanlines of a particular size and means nothing once
 * the size is different. */
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
        return false;
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
        mode0_prog(xregs); /* all zeros: the terminal across the whole canvas */
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
