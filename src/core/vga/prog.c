/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/prog.h"
#include "core/vga/mode/mode0.h"
#include "core/vga/mode/mode1.h"
#include "core/vga/mode/mode2.h"
#include "core/vga/mode/mode3.h"
#include "core/vga/mode/mode4.h"
#include "core/vga/mode/mode5.h"
#include <stddef.h>

#pragma GCC push_options
#pragma GCC optimize("O3")

static vga_prog_t vga_prog[VGA_PROG_MAX];

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

void vga_prog_load_row(int16_t scanline, const vga_prog_t *row)
{
    if (scanline >= 0 && scanline < VGA_PROG_MAX)
        vga_prog[scanline] = *row;
}

void vga_prog_set_highest(int16_t scanline)
{
    vga_highest_scanline = scanline;
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

vga_fill_fn_t vga_mode_fill_fn(uint8_t mode, uint16_t options)
{
    switch (mode)
    {
    case 0: return mode0_fill_fn(options);
    case 1: return mode1_fill_fn(options);
    case 2: return mode2_fill_fn(options);
    case 3: return mode3_fill_fn(options);
    default: return NULL;
    }
}

vga_sprite_fn_t vga_mode_sprite_fn(uint8_t mode, uint16_t options)
{
    switch (mode)
    {
    case 4: return mode4_sprite_fn(options);
    case 5: return mode5_sprite_fn(options);
    default: return NULL;
    }
}

static void vga_prog_row_set(vga_prog_t *row, int16_t plane, vga_fill_fn_t fill_fn,
                             uint8_t mode, uint16_t options, uint16_t config_ptr)
{
    row->fill_fn[plane] = fill_fn;
    row->fill_mode[plane] = mode;
    row->fill_options[plane] = options;
    row->fill_config[plane] = config_ptr;
}

bool vga_prog_row_fill(vga_prog_t *row, int16_t plane, uint8_t mode,
                       uint16_t options, uint16_t config_ptr)
{
    const vga_fill_fn_t fill_fn = vga_mode_fill_fn(mode, options);
    if (!fill_fn)
        return false;
    vga_prog_row_set(row, plane, fill_fn, mode, options, config_ptr);
    return true;
}

bool vga_prog_fill(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                   uint16_t config_ptr, uint8_t mode, uint16_t options)
{
    const vga_fill_fn_t fill_fn = vga_mode_fill_fn(mode, options);
    if (!fill_fn || vga_canvas_is_console())
        return false;
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
        vga_prog_row_set(&vga_prog[i], plane, fill_fn, mode, options, config_ptr);
    return true;
}

bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                        uint16_t config_ptr, uint8_t mode, uint16_t options)
{
    const vga_fill_fn_t fill_fn = vga_mode_fill_fn(mode, options);
    if (!fill_fn)
        return false;
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (uint16_t i = 0; i < VGA_PROG_MAX; i++)
        for (uint16_t j = 0; j < SCANVIDEO_PLANE_COUNT; j++)
            if (vga_prog[i].fill_fn[j] && vga_prog[i].fill_mode[j] == mode)
                vga_prog[i].fill_fn[j] = NULL;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
        vga_prog_row_set(&vga_prog[i], plane, fill_fn, mode, options, config_ptr);
    return true;
}

bool vga_prog_sprite(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                     uint16_t config_ptr, uint16_t length,
                     uint8_t mode, uint16_t options)
{
    const vga_sprite_fn_t sprite_fn = vga_mode_sprite_fn(mode, options);
    if (!sprite_fn || vga_canvas_is_console())
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
