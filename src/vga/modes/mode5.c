/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/mode5.h"
#include "sys/mem.h"
#include "sys/vga.h"
#include "term/color.h"

#pragma GCC push_options
#pragma GCC optimize("O3")

typedef struct
{
    int16_t x_pos_px;
    int16_t y_pos_px;
    uint16_t xram_sprite_ptr;
    uint16_t palette_ptr;
} mode5_sprite_t;

static inline const uint16_t *
mode5_get_palette(uint16_t palette_ptr, int16_t bpp)
{
    if (!(palette_ptr & 1) &&
        palette_ptr <= 0x10000 - sizeof(uint16_t) * (1 << bpp))
        return (const uint16_t *)&xram[palette_ptr];
    if (bpp == 1)
        return color_2;
    return color_256;
}

static inline __attribute__((always_inline)) void
mode5_render(int16_t scanline, int16_t width, uint16_t *rgb,
             uint16_t config_ptr, uint16_t length,
             int16_t sprite_size, int16_t bpp)
{
    const mode5_sprite_t *sprites = (const mode5_sprite_t *)&xram[config_ptr];
    const int16_t bytes_per_row = sprite_size * bpp / 8;
    const uint16_t sprite_data_size = sprite_size * bytes_per_row;

    for (uint16_t i = 0; i < length; i++)
    {
        int16_t tex_y = scanline - sprites[i].y_pos_px;
        if (tex_y < 0 || tex_y >= sprite_size)
            continue;

        int16_t x_start = sprites[i].x_pos_px;
        int16_t tex_x = 0;
        int16_t size_x = sprite_size;

        if (x_start < 0)
        {
            tex_x = -x_start;
            size_x += x_start;
            x_start = 0;
        }
        if (x_start + size_x > width)
            size_x = width - x_start;
        if (size_x <= 0)
            continue;

        if (sprites[i].xram_sprite_ptr > 0x10000 - sprite_data_size)
            continue;

        const uint16_t *palette = mode5_get_palette(sprites[i].palette_ptr, bpp);
        const uint8_t *row_data =
            (const uint8_t *)&xram[sprites[i].xram_sprite_ptr + tex_y * bytes_per_row];
        uint16_t *dst = rgb + x_start;

        for (int16_t px = tex_x; px < tex_x + size_x; px++)
        {
            uint8_t idx;
            if (bpp == 1)
                idx = (row_data[px / 8] >> (7 - (px & 7))) & 0x01;
            else if (bpp == 2)
                idx = (row_data[px / 4] >> (6 - 2 * (px & 3))) & 0x03;
            else if (bpp == 4)
                idx = (px & 1) ? (row_data[px / 2] & 0x0F) : (row_data[px / 2] >> 4);
            else
                idx = row_data[px];

            uint16_t color = palette[idx];
            if (color & (1 << 5))
                *dst = color;
            dst++;
        }
    }
}

static void
mode5_render_1bpp_8x8(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 8, 1);
}

static void
mode5_render_2bpp_8x8(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 8, 2);
}

static void
mode5_render_4bpp_8x8(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 8, 4);
}

static void
mode5_render_8bpp_8x8(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 8, 8);
}

static void
mode5_render_1bpp_16x16(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 16, 1);
}

static void
mode5_render_2bpp_16x16(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 16, 2);
}

static void
mode5_render_4bpp_16x16(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 16, 4);
}

static void
mode5_render_8bpp_16x16(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 16, 8);
}

bool mode5_prog(uint16_t *xregs)
{
    const uint16_t attributes = xregs[2];
    const uint16_t config_ptr = xregs[3];
    const uint16_t length = xregs[4];
    const int16_t plane = xregs[5];
    const int16_t scanline_begin = xregs[6];
    const int16_t scanline_end = xregs[7];

    if (config_ptr & 1)
        return false;

    if (config_ptr > 0x10000 - sizeof(mode5_sprite_t) * length)
        return false;

    void (*render_fn)(int16_t, int16_t, uint16_t *, uint16_t, uint16_t);
    switch (attributes)
    {
    case 0:
        render_fn = mode5_render_1bpp_8x8;
        break;
    case 1:
        render_fn = mode5_render_2bpp_8x8;
        break;
    case 2:
        render_fn = mode5_render_4bpp_8x8;
        break;
    case 3:
        render_fn = mode5_render_8bpp_8x8;
        break;
    case 8:
        render_fn = mode5_render_1bpp_16x16;
        break;
    case 9:
        render_fn = mode5_render_2bpp_16x16;
        break;
    case 10:
        render_fn = mode5_render_4bpp_16x16;
        break;
    case 11:
        render_fn = mode5_render_8bpp_16x16;
        break;
    default:
        return false;
    };

    return vga_prog_sprite(plane, scanline_begin, scanline_end, config_ptr, length, render_fn);
}

#pragma GCC pop_options
