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

/* The rows the savestate carries, each 3 planes of 12 bytes: the fill's
 * mode, options and config, then the sprites' mode, options, config and
 * length. The rows past them are never booked. */
#define PROG_SST_LINES 480

static bool prog_sst_highest_ok(sst_cursor_t *c)
{
    if (vga_highest_scanline < 0 || vga_highest_scanline > PROG_SST_LINES)
        return sst_refuse(c, "a highest scanline past the canvas");
    return true;
}

static void prog_sst_put_rows(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    for (int16_t line = 0; line < PROG_SST_LINES; line++)
    {
        const vga_prog_t *row = &vga_prog[line];
        for (int16_t plane = 0; plane < SCANVIDEO_PLANE_COUNT; plane++)
        {
            uint8_t mode = row->fill_fn[plane] ? row->fill_mode[plane] : VGA_MODE_NONE;
            uint16_t attr = row->fill_fn[plane] ? row->fill_options[plane] : 0;
            sst_put_u8(c, mode);
            sst_put_u16(c, attr);
            sst_put_u16(c, row->fill_config[plane]);
            if (!vga_mode_sprite_id(row->sprite_fn[plane], &mode, &attr))
                mode = VGA_MODE_NONE, attr = 0;
            sst_put_u8(c, mode);
            sst_put_u16(c, attr);
            sst_put_u16(c, row->sprite_config[plane]);
            sst_put_u16(c, row->sprite_length[plane]);
        }
    }
}

/* Each config is bounded as its mode's prog bounds it, because the renderer
 * reads it from xram unchecked. */
static bool prog_sst_get_rows(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    for (int16_t line = PROG_SST_LINES; line < VGA_PROG_MAX; line++)
        vga_prog[line] = (vga_prog_t){0};
    for (int16_t line = 0; line < PROG_SST_LINES; line++)
    {
        vga_prog_t row = {0};
        for (int16_t plane = 0; plane < SCANVIDEO_PLANE_COUNT; plane++)
        {
            uint8_t mode = sst_get_u8(c);
            uint16_t attr = sst_get_u16(c);
            uint16_t config = sst_get_u16(c);
            if (mode == VGA_MODE_NONE)
                row.fill_config[plane] = config;
            else if (!vga_mode_config_ok(mode, attr, config, 0) ||
                     !vga_prog_row_fill(&row, plane, mode, attr, config))
                return sst_refuse(c, "a fill this build cannot draw");
            mode = sst_get_u8(c);
            attr = sst_get_u16(c);
            config = sst_get_u16(c);
            uint16_t length = sst_get_u16(c);
            if (mode != VGA_MODE_NONE)
            {
                row.sprite_fn[plane] = vga_mode_sprite_fn(mode, attr);
                if (!row.sprite_fn[plane] || !vga_mode_config_ok(mode, attr, config, length))
                    return sst_refuse(c, "sprites this build cannot draw");
            }
            row.sprite_config[plane] = config;
            row.sprite_length[plane] = length;
        }
        vga_prog[line] = row;
    }
    return sst_ok(c);
}

#define PROG_SST(X)                                                    \
    X(I16, vga_highest_scanline)                                       \
    X(VALIDATE, prog_sst_highest_ok)                                   \
    X(FN, prog_sst_put_rows, prog_sst_get_rows,                        \
      PROG_SST_LINES * SCANVIDEO_PLANE_COUNT * 12, 1)                  \
    X(IN_FN, vga_prog)
SST_TABLE(prog, PROG_SST)

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

bool vga_mode_config_ok(uint8_t mode, uint16_t options, uint16_t config_ptr,
                        uint16_t length)
{
    switch (mode)
    {
    case 1: return mode1_config_ok(config_ptr);
    case 2: return mode2_config_ok(config_ptr);
    case 3: return mode3_config_ok(config_ptr);
    case 4: return mode4_config_ok(options, config_ptr, length);
    case 5: return mode5_config_ok(options, config_ptr, length);
    default: return true; /* mode 0 bounds its rows as it draws them */
    }
}

bool vga_mode_sprite_id(vga_sprite_fn_t fn, uint8_t *mode, uint16_t *attributes)
{
    if (!fn)
    {
        *mode = VGA_MODE_NONE;
        *attributes = 0;
        return true;
    }
    if (mode4_sprite_attr(fn, attributes))
    {
        *mode = 4;
        return true;
    }
    if (mode5_sprite_attr(fn, attributes))
    {
        *mode = 5;
        return true;
    }
    return false;
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
