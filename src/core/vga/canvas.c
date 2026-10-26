/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/vga.h"
#include "core/vga/mode/mode0.h"
#include "core/api/xreg.h"
#include "core/term/term.h"

/* canvas_w and canvas_h cache the selected canvas's size, so that the getters
 * below, which run on every scanline, do not each look it up again. */
static vga_canvas_t canvas_code = vga_canvas_console;
static int16_t canvas_w = 640, canvas_h = 480;
static int16_t console_w = 640, console_h = 480;

void vga_console_size(int *w, int *h)
{
    *w = console_w;
    *h = console_h;
}

void vga_set_console_size(int w, int h)
{
    console_w = (int16_t)w;
    console_h = (int16_t)h;
}

/* The savestate load restores the scanline table and the terminal's first line
 * after this call, so this function must not reset the table or re-program the
 * terminal the way vga_canvas_select does. */
bool vga_canvas_load(uint16_t canvas)
{
    int w, h;
    if (!vga_canvas_geometry(canvas, &w, &h))
        return false;
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
    int w, h;
    if (!vga_canvas_geometry(canvas, &w, &h))
        return false;
    canvas_code = (vga_canvas_t)canvas;
    canvas_w = (int16_t)w;
    canvas_h = (int16_t)h;
    vga_canvas_reset();
    vga_canvas_publish(canvas_code);
    if (canvas_code == vga_canvas_console)
    {
        uint16_t xregs[8] = {0};
        mode0_prog(xregs); /* all zeros: the tallest terminal that fits, centered */
    }
    return true;
}

void vga_console_reset(void)
{
    vga_canvas_select(vga_canvas_console);
    term_RIS_no_clear();
    xreg1_clear();
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

/* A 320 wide canvas is scanned out with its lines doubled, so one row of
 * graphics spans two lines of timing and the beam stays in step with the
 * 6502. The canvas sits at the top of the frame and the porch takes up the
 * slack, rather than modelling the letterbox the 4:3 views letterbox with. */
int16_t vga_canvas_y_scale(void)
{
    return canvas_w == 320 ? 2 : 1;
}

void vga_canvas_size(int *w, int *h)
{
    *w = canvas_w;
    *h = canvas_h;
}
