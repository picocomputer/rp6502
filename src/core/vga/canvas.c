/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/canvas.h"
#include "core/vga/vga.h"
#include "core/vga/mode/mode0.h"
#include "core/api/xreg.h"
#include "core/term/cells.h"

/* canvas_w and canvas_h cache the selected canvas's size, so that the getters
 * below, which run on every scanline, do not each look it up again. */
static vga_canvas_t canvas_code = vga_canvas_console;
static int16_t canvas_w = 640, canvas_h = 480;
static int16_t console_w = 640, console_h = 480;

/* The savestate load puts the canvas back without what vga_canvas_select
 * does to the scanline table and the terminal, because their own rows
 * restore them. */
static bool canvas_load(uint16_t canvas)
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

/* The console's height is carried because the host chooses it, and a
 * program left on the console canvas would come back with its terminal
 * laid out for another size. The load sets the size it carries in place of
 * the host's, and stops and resets keep it, so it stays for the rest of the
 * session. */
static bool canvas_sst_console_ok(sst_cursor_t *c)
{
    if (!vga_console_size_ok(console_w, console_h))
        return sst_refuse(c, "a console no host gives");
    return true;
}

/* The canvas loads after the console's size, which the console canvas
 * takes, and after the cells, which it publishes for the size it shows. */
static void canvas_sst_put_code(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u16(c, (uint16_t)canvas_code);
}

static bool canvas_sst_get_code(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint16_t canvas = sst_get_u16(c);
    if (!sst_ok(c))
        return false;
    if (!canvas_load(canvas))
        return sst_refuse(c, "a canvas this build lacks");
    cells_publish();
    return true;
}

#define CANVAS_SST(X)                                                  \
    X(I16, console_h)                                                  \
    X(SKIP, console_w, "every console is 640 wide")                    \
    X(VALIDATE, canvas_sst_console_ok)                                 \
    X(FN, canvas_sst_put_code, canvas_sst_get_code, 2, 1)              \
    X(IN_FN, canvas_code)                                              \
    X(SKIP, canvas_w, "derived from the canvas by its load")           \
    X(SKIP, canvas_h, "derived from the canvas by its load")
SST_TABLE(canvas, CANVAS_SST)

/* A refused load can leave a console no host gives, which the console's
 * reset would lay out. Every host gives 640x480. */
void canvas_reset(void)
{
    if (!vga_console_size_ok(console_w, console_h))
        vga_set_console_size(640, 480);
}

void vga_console_size(int *w, int *h)
{
    *w = console_w;
    *h = console_h;
}

void vga_display_size(int *w, int *h)
{
    vga_console_size(w, h);
}

void vga_set_console_size(int w, int h)
{
    console_w = (int16_t)w;
    console_h = (int16_t)h;
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
    cells_RIS_no_clear();
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
