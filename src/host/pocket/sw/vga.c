/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mmio.h"
#include "vga.h"

#include "core/vga/pixel_format.h"

#include "core/hid/tablet.h"
#include "core/term/font.h"
#include "core/vga/mode/mode5.h"
#include "mode6_shapes.h"

static int16_t vga_highest_scanline;

bool vga_connected(void)
{
    return true;
}

bool vga_console_busy(void)
{
    return false;
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

/* The scanline table itself is the fabric's, which fab.c carries; these
 * are this file's notes on it. A plane past the fabric's would write past
 * VID_XPROG's planes. */
static bool vga_fabric_check(sst_cursor_t *c)
{
    if (vga_highest_scanline < 0 || vga_highest_scanline > VGA_PROG_MAX ||
        vga_mode0_plane < 0 || vga_mode0_plane >= SCANVIDEO_PLANE_COUNT)
        return sst_refuse(c, "the scanline table's notes are out of range");
    return true;
}

/* VID_VSYNC_LINE is written, not read, and is computed from the canvas,
 * which loads before this row. */
static void vga_vsync_put(sst_cursor_t *c, unsigned flags)
{
    (void)c;
    (void)flags;
}

static bool vga_vsync_get(sst_cursor_t *c, unsigned flags)
{
    (void)c;
    (void)flags;
    vga_publish_vsync();
    return true;
}

/* The pointer, mode6.sv's (core/vga/mode/mode6.h): the program's sprite
 * array, or $FFFF for the system's shapes. A system shape is loaded into
 * the copy of mode6.sv's image that is not showing, at the size of the
 * canvas, and then named, which mode6.sv takes as the next frame starts. A
 * frame must have started since, so the copy showing is never the one
 * written. */
static uint16_t vga_ptr_config = 0xFFFF;
static uint8_t vga_ptr_loaded; /* the system shape showing, 0 for none */
static bool vga_ptr_small;     /* at the 320-wide canvases' size */
static uint8_t vga_ptr_copy;   /* the copy it is in */
static bool vga_ptr_flipping;  /* named since the frame vga_ptr_flipped */
static uint32_t vga_ptr_flipped;

/* mode6.sv's config is written again after a load, and the system shape
 * loaded again, into the copy that is not named. */
static void vga_ptr_put(sst_cursor_t *c, unsigned flags)
{
    (void)c;
    (void)flags;
}

static bool vga_ptr_get(sst_cursor_t *c, unsigned flags)
{
    (void)c;
    (void)flags;
    VID_PTR_CONFIG = vga_ptr_config;
    vga_ptr_loaded = 0;
    vga_ptr_flipping = false;
    return true;
}

#define VGA_FABRIC_SST(X)                                               \
    X(I16, vga_highest_scanline)                                        \
    X(U32S, vga_mode0_mask)                                             \
    X(I16, vga_mode0_plane)                                             \
    X(VALIDATE, vga_fabric_check)                                       \
    X(FN, vga_vsync_put, vga_vsync_get, 0, 1)                           \
    X(U16, vga_ptr_config)                                              \
    X(FN, vga_ptr_put, vga_ptr_get, 0, 1)                               \
    X(SKIP, vga_ptr_loaded, "loaded again after a load")               \
    X(SKIP, vga_ptr_small, "loaded again after a load")                \
    X(SKIP, vga_ptr_copy, "loaded again after a load")                 \
    X(SKIP, vga_ptr_flipping, "loaded again after a load")             \
    X(SKIP, vga_ptr_flipped, "loaded again after a load")
SST_TABLE(vga_fabric, VGA_FABRIC_SST)

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

/* Loads system shape `shape` at the canvas's size into the copy of
 * mode6.sv's image that is not showing, as core/vga/mode/mode6.c copies
 * it: its image a word at a time, the first byte in the low bits, as XRAM
 * reads, and its hot spot and size. */
static void vga_ptr_load(uint8_t shape, bool small)
{
    const mode6_shape_t *m = &MODE6_SHAPES[shape][small];
    const uint32_t bytes = mode5_custom_bytes(m->width_height, 2);
    const uint8_t copy = !vga_ptr_copy;
    for (uint32_t w = 0; w * 4 < bytes; w++)
    {
        uint32_t word = 0;
        for (uint32_t b = 0; b < 4 && w * 4 + b < bytes; b++)
            word |= (uint32_t)MODE6_DATA[m->offset + w * 4 + b] << (8 * b);
        VID_PTR_IMG(copy, w) = word;
    }
    VID_PTR_SYS(copy) = m->hot_x | (uint32_t)m->hot_y << 8 |
                        (uint32_t)m->width_height << 16;
    vga_ptr_copy = copy;
    vga_ptr_loaded = shape;
    vga_ptr_small = small;
}

/* As core/vga/vga.c's: the host's own cursor, where it shows one, stands
 * for the system's shapes. A shape not yet loaded shows the one that is
 * until a frame has started since the last was named. The frame is read
 * after the name is written, so a frame that starts between the two only
 * holds the next load back a frame longer. */
void vga_set_pointer(uint8_t shape, int16_t x, int16_t y)
{
    VID_PTR_WHERE = (uint16_t)x | (uint32_t)(uint16_t)y << 16;
    bool loaded = false;
    if (vga_ptr_config == 0xFFFF)
    {
        if (tablet_host_cursor_shown() || shape >= MODE6_SHAPE_COUNT)
            shape = 0;
        const bool small = vga_canvas_width() <= 320;
        if (shape && (shape != vga_ptr_loaded || small != vga_ptr_small))
        {
            if (vga_ptr_flipping && VID_FRAME == vga_ptr_flipped)
                shape = vga_ptr_loaded;
            else
            {
                vga_ptr_load(shape, small);
                loaded = true;
            }
        }
    }
    VID_PTR_SHAPE = shape | (uint32_t)vga_ptr_copy << 8;
    if (loaded)
    {
        vga_ptr_flipping = true;
        vga_ptr_flipped = VID_FRAME;
    }
}

/* The shape is taken off with a new config, as they latch together, so no
 * frame draws a shape number of one kind as the other. The tablet's next
 * vga_set_pointer names it again. */
bool vga_prog_pointer(uint16_t config_ptr)
{
    if (config_ptr != vga_ptr_config)
        VID_PTR_SHAPE = (uint32_t)vga_ptr_copy << 8;
    vga_ptr_config = config_ptr;
    VID_PTR_CONFIG = config_ptr;
    return true;
}

void vga_set_code_page(uint16_t cp)
{
    font_set_code_page(cp);
}

void vga_load_code_page(uint16_t cp)
{
    font_load_code_page(cp);
}

