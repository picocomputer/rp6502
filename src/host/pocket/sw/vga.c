/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mmio.h"
#include "vga.h"

#include "core/vga/pixel_format.h"

#include "core/term/cells.h"
#include "core/term/font.h"

static int16_t vga_highest_scanline;

bool vga_connected(void)
{
    return true;
}

bool vga_prog_valid(int16_t plane, int16_t scanline_begin,
                    int16_t *scanline_end)
{
    if (!*scanline_end)
        *scanline_end = vga_canvas_height();
    if (plane < 0 || plane >= SCANVIDEO_PLANE_COUNT ||
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

/* Each line programmed by the last successful vga_prog_exclusive call has
 * a bit here until a vga_prog_fill on the same plane replaces the line or
 * vga_canvas_reset clears the table. */
#define VGA_MASK_WORDS (VGA_PROG_MAX / 32)
static uint32_t vga_mode0_mask[VGA_MASK_WORDS];
static int16_t vga_mode0_plane;

/* The scanline table is prog.sv's: a word for a plane's fill and one for
 * its sprites, each the mode and the options word of its XREG call under
 * an enable bit. */
static uint32_t vga_table_word(uint8_t mode, uint16_t options)
{
    return 0x80000000u | ((uint32_t)(mode & 7) << 16) | options;
}

/* Only the terminal books this way, and its mode and options are 0. */
bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin,
                        int16_t scanline_end, uint16_t config_ptr,
                        uint8_t mode, uint16_t options)
{
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (int16_t i = 0; i < VGA_PROG_MAX; i++)
        if (vga_mode0_mask[i >> 5] & (1u << (i & 31)))
            VID_XPROG(i, vga_mode0_plane, 0) = 0;
    for (int16_t i = 0; i < VGA_MASK_WORDS; i++)
        vga_mode0_mask[i] = 0;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        VID_XPROG(i, plane, 0) = vga_table_word(mode, options);
        VID_XPROG(i, plane, 1) = config_ptr;
        vga_mode0_mask[i >> 5] |= 1u << (i & 31);
    }
    vga_mode0_plane = plane;
    vga_publish_vsync();
    return true;
}

bool vga_prog_fill(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                   uint16_t config_ptr, uint8_t mode, uint16_t options)
{
    if (vga_canvas_is_console())
        return false;
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        VID_XPROG(i, plane, 0) = vga_table_word(mode, options);
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
                     uint8_t mode, uint16_t options)
{
    if (vga_canvas_is_console())
        return false;
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        VID_XPROG(i, plane, 2) = vga_table_word(mode, options);
        VID_XPROG(i, plane, 3) = ((uint32_t)length << 16) | config_ptr;
    }
    vga_publish_vsync();
    return true;
}

void vga_canvas_reset(void)
{
    for (int16_t i = 0; i < VGA_PROG_MAX; i++)
        for (int16_t p = 0; p < SCANVIDEO_PLANE_COUNT; p++)
            for (int16_t w = 0; w < VID_XPROG_WORDS; w++)
                VID_XPROG(i, p, w) = 0;
    for (int16_t i = 0; i < VGA_MASK_WORDS; i++)
        vga_mode0_mask[i] = 0;
    vga_highest_scanline = 0;
    vga_publish_vsync();
}

void vga_canvas_publish(vga_canvas_t canvas)
{
    VID_CANVAS = canvas;
}

/* What this file keeps of the scanline table, for vid.c's savestate row.
 * The table itself is the fabric's, which fab.c carries. The load comes
 * after the canvas's, which VID_VSYNC_LINE is computed from. */
void vga_table_save(sst_cursor_t *c)
{
    sst_put_i16(c, vga_highest_scanline);
    for (int i = 0; i < VGA_MASK_WORDS; i++)
        sst_put_u32(c, vga_mode0_mask[i]);
    sst_put_i16(c, vga_mode0_plane);
}

bool vga_table_load(sst_cursor_t *c)
{
    int16_t highest = sst_get_i16(c);
    uint32_t mask[VGA_MASK_WORDS];
    for (int i = 0; i < VGA_MASK_WORDS; i++)
        mask[i] = sst_get_u32(c);
    int16_t plane = sst_get_i16(c);
    if (!sst_ok(c) || highest < 0 || highest > VGA_PROG_MAX || plane < 0
        || plane >= SCANVIDEO_PLANE_COUNT)
        return false;
    vga_highest_scanline = highest;
    for (int i = 0; i < VGA_MASK_WORDS; i++)
        vga_mode0_mask[i] = mask[i];
    vga_mode0_plane = plane;
    vga_publish_vsync();
    return true;
}

/* font.c's tables are linked onto the font memories, so these write them. */
void vga_set_code_page(uint16_t cp)
{
    font_set_code_page(cp);
}

void vga_load_code_page(uint16_t cp)
{
    font_load_code_page(cp);
}

