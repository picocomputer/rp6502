/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See prog.h. The bookings a mode makes, and the table the renderer walks.
 */

#include "core/vga/prog.h"
#include <stddef.h>

/* -O3 here on the firmware, where this table is read once per scanline at
 * 25 MHz pixel rate and the build is otherwise -Os. */
#pragma GCC push_options
#pragma GCC optimize("O3")

static vga_prog_t vga_prog[VGA_PROG_MAX];

/* Highest scanline any program renders; vsync fires here. */
static int16_t vga_highest_scanline;

const vga_prog_t *vga_prog_row(int16_t scanline)
{
    return &vga_prog[scanline];
}

void vga_prog_reset(void)
{
    for (uint16_t i = 0; i < VGA_PROG_MAX; i++)
        vga_prog[i] = (vga_prog_t){0};
    vga_highest_scanline = 0;
}

int16_t vga_prog_highest(void)
{
    return vga_highest_scanline;
}

bool vga_prog_valid(int16_t plane, int16_t scanline_begin, int16_t *scanline_end)
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

bool vga_prog_fill(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                   uint16_t config_ptr, vga_fill_fn_t fill_fn)
{
    if (vga_canvas_is_console()) /* graphics modes need a canvas */
        return false;
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        vga_prog[i].fill_config[plane] = config_ptr;
        vga_prog[i].fill_fn[plane] = fill_fn;
    }
    return true;
}

bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                        uint16_t config_ptr, vga_fill_fn_t fill_fn)
{
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    /* Remove every prior instance of this fill_fn (term re-programs on resize). */
    for (uint16_t i = 0; i < VGA_PROG_MAX; i++)
        for (uint16_t j = 0; j < SCANVIDEO_PLANE_COUNT; j++)
            if (vga_prog[i].fill_fn[j] == fill_fn)
                vga_prog[i].fill_fn[j] = NULL;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        vga_prog[i].fill_config[plane] = config_ptr;
        vga_prog[i].fill_fn[plane] = fill_fn;
    }
    return true;
}

bool vga_prog_sprite(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                     uint16_t config_ptr, uint16_t length, vga_sprite_fn_t sprite_fn)
{
    if (vga_canvas_is_console())
        return false;
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        vga_prog[i].sprite_config[plane] = config_ptr;
        vga_prog[i].sprite_length[plane] = length;
        vga_prog[i].sprite_fn[plane] = sprite_fn;
    }
    return true;
}

#pragma GCC pop_options
