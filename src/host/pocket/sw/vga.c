/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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
        vga_highest_scanline = *scanline_end;
    return true;
}

int16_t vga_prog_highest(void)
{
    return vga_highest_scanline;
}

/* The vsync pulse advances the RIA frame counter and sets the vsync
 * interrupt pending, and VID_VSYNC_LINE is the scanline it fires on. prog.sv
 * stores the line without a range check, so the value written is the one
 * vga_vsync_line limits to the canvas, and it is written again after every
 * booking and every canvas reset because either can change it. */
static void vga_publish_vsync(void)
{
    VID_VSYNC_LINE = (uint32_t)vga_vsync_line();
}

/* The soft CPU cannot read an entry back from the scanline table, so each
 * line programmed by the last successful vga_prog_exclusive call has a bit
 * here until a vga_prog_fill on the same plane replaces the line or
 * vga_canvas_reset clears the table. */
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
    vga_publish_vsync();
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
    vga_publish_vsync();
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
    vga_publish_vsync();
    return true;
}

void vga_canvas_reset(void)
{
    for (int16_t i = 0; i < 512; i++)
        for (int16_t p = 0; p < 3; p++)
            for (int16_t w = 0; w < 4; w++)
                VID_XPROG(i, p, w) = 0;
    for (int16_t i = 0; i < 16; i++)
        vga_mode0_mask[i] = 0;
    vga_highest_scanline = 0;
    vga_publish_vsync();
}

void vga_canvas_publish(vga_canvas_t canvas)
{
    VID_CANVAS = canvas;
}

/* The blob does not contain VID_CANVAS or VID_VSYNC_LINE, so after a
 * restore they hold their power-on values or the values written before the
 * restore. The blob does contain the scanline table and the firmware state
 * both values come from. vga_canvas_select is not used here because it
 * clears the scanline table the blob has just restored. */
void vga_restore(void)
{
    VID_CANVAS = (uint32_t)vga_get_canvas();
    vga_publish_vsync();
}

void vga_set_code_page(uint16_t cp)
{
    font_set_code_page(cp);
}
void vga_load_code_page(uint16_t cp)
{
    vga_set_code_page(cp);
}

