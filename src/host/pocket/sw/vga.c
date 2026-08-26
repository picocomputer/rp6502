/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Both sides of the VGA contract on one machine, core/vga/vga.c's shape.
 * There is no g_prog array: the RTL scanline program is the storage, and
 * entries are published tagged with the mode and attribute the
 * dispatcher announced, because a fill-function pointer means nothing to
 * hardware.
 */

#include "font.h"
#include "mmio.h"
#include "vga.h"
#include "vid.h"

#include "core/term/term.h"

static int16_t vga_highest_scanline;
static uint8_t vga_pub_mode;
static uint16_t vga_pub_attr;

bool vga_connected(void)
{
    return true;
}

uint8_t vga_get_display_type(void)
{
    return 1;
}

int16_t vga_vsync_scanline(void)
{
    return vga_highest_scanline;
}

/* The fabric renders, so it is told which mode before the planes arrive. */
void vga_mode_begin(uint8_t mode, uint16_t attr)
{
    vga_pub_mode = mode;
    vga_pub_attr = attr;
}

bool vga_prog_valid(int16_t plane, int16_t scanline_begin,
                    int16_t *scanline_end)
{
    if (!*scanline_end)
        *scanline_end = vga_canvas_height();
    if (plane < 0 || plane >= 3 ||
        scanline_begin < 0 || *scanline_end > vga_canvas_height() ||
        *scanline_end - scanline_begin < 1)
        return false;
    if (*scanline_end > vga_highest_scanline)
    {
        vga_highest_scanline = *scanline_end;
        VID_VSYNC_LINE = (uint32_t)vga_highest_scanline;
    }
    return true;
}

/* The table is write-only from the bus, so the exclusive sweep's one bit
 * per line lives here. */
static uint32_t vga_mode0_mask[16];
static int16_t vga_mode0_plane;

bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin,
                        int16_t scanline_end, uint16_t config_ptr,
                        bool (*fill_fn)(int16_t, int16_t, int16_t,
                                        uint16_t *, uint16_t))
{
    (void)fill_fn;
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (int16_t i = 0; i < 512; i++)
        if (vga_mode0_mask[i >> 5] & (1u << (i & 31)))
            VID_XPROG(i, vga_mode0_plane, 0) = 0;
    for (int16_t i = 0; i < 16; i++)
        vga_mode0_mask[i] = 0;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        VID_XPROG(i, plane, 0) = 0x80000000u;
        VID_XPROG(i, plane, 1) = config_ptr;
        vga_mode0_mask[i >> 5] |= 1u << (i & 31);
    }
    vga_mode0_plane = plane;
    return true;
}

bool vga_prog_fill(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                   uint16_t config_ptr,
                   bool (*fill_fn)(int16_t, int16_t, int16_t,
                                   uint16_t *, uint16_t))
{
    (void)fill_fn;
    if (vga_canvas_is_console())
        return false;
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        VID_XPROG(i, plane, 0) = 0x80000000u
            | ((uint32_t)(vga_pub_mode & 7) << 16) | vga_pub_attr;
        VID_XPROG(i, plane, 1) = config_ptr;
    }
    if (plane == vga_mode0_plane)
        for (int16_t i = scanline_begin; i < scanline_end; i++)
            vga_mode0_mask[i >> 5] &= ~(1u << (i & 31));
    return true;
}

bool vga_prog_sprite(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                     uint16_t config_ptr, uint16_t length,
                     void (*sprite_fn)(int16_t, int16_t, uint16_t *,
                                       uint16_t, uint16_t))
{
    (void)sprite_fn;
    if (vga_canvas_is_console())
        return false;
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        VID_XPROG(i, plane, 2) = 0x80000000u
            | ((uint32_t)(vga_pub_mode & 7) << 16) | vga_pub_attr;
        VID_XPROG(i, plane, 3) = ((uint32_t)length << 16) | config_ptr;
    }
    return true;
}

/* The fabric holds the programming, so forgetting it is a sweep of the
 * scanline registers and the mode 0 masks -- and this machine's own record of
 * how far down the picture goes, which is not the shared table's. */
void vga_canvas_reset(void)
{
    for (int16_t i = 0; i < 512; i++)
        for (int16_t p = 0; p < 3; p++)
            for (int16_t w = 0; w < 4; w++)
                VID_XPROG(i, p, w) = 0;
    for (int16_t i = 0; i < 16; i++)
        vga_mode0_mask[i] = 0;
    vga_highest_scanline = 0;
}

/* The fabric sizes the picture, so it is told. */
void vga_canvas_publish(vga_canvas_t canvas)
{
    VID_CANVAS = canvas;
}

/* A wake reconfigures the part, so these two come back at their
 * power-on values -- console, and a vsync line of 480 -- while the
 * blob has brought back the scanline table they belong to and the
 * shadows above that say what they were. Not vga_canvas_select: that
 * sweeps the table, which is exactly what the blob just restored.
 *
 * The canvas is the whole picture. It is the scaler mode the raster
 * names at the end of every line, and it is also the width the fill
 * engines are given a line's worth of clocks to produce: a 320-wide
 * program woken onto a 640-wide canvas is asked for twice the pixels
 * in the same time, does not finish, and never flips its bank. That is
 * a black screen over a program that is still running. */
void vga_restore(void)
{
    VID_CANVAS = (uint32_t)vga_get_canvas();
    VID_VSYNC_LINE = (uint32_t)vga_highest_scanline;
}

/* One font store, and the glyphs in it are this page's. */
void vga_set_code_page(uint16_t cp)
{
    font_set_code_page(cp);
}
