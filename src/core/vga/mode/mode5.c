/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/mode/mode5.h"
#include "core/sys/xram.h"
#include "core/vga/vga.h"
#include "core/term/color.h"

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
    const int16_t bytes_per_row = sprite_size * bpp / 8;
    const uint32_t sprite_data_size = (uint32_t)sprite_size * bytes_per_row;
    if (sprite_data_size > 0x10000)
        return;

    const mode5_sprite_t *sprites = (const mode5_sprite_t *)&xram[config_ptr];

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

static void
mode5_render_1bpp_32x32(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 32, 1);
}

static void
mode5_render_2bpp_32x32(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 32, 2);
}

static void
mode5_render_4bpp_32x32(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 32, 4);
}

static void
mode5_render_8bpp_32x32(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 32, 8);
}

static void
mode5_render_1bpp_64x64(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 64, 1);
}

static void
mode5_render_2bpp_64x64(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 64, 2);
}

static void
mode5_render_4bpp_64x64(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 64, 4);
}

static void
mode5_render_8bpp_64x64(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 64, 8);
}

static void
mode5_render_1bpp_128x128(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 128, 1);
}

static void
mode5_render_2bpp_128x128(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 128, 2);
}

static void
mode5_render_4bpp_128x128(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 128, 4);
}

static void
mode5_render_8bpp_128x128(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 128, 8);
}

static void
mode5_render_1bpp_256x256(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 256, 1);
}

static void
mode5_render_2bpp_256x256(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 256, 2);
}

static void
mode5_render_4bpp_256x256(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 256, 4);
}

static void
mode5_render_8bpp_256x256(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 256, 8);
}

static void
mode5_render_1bpp_512x512(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 512, 1);
}

static void
mode5_render_2bpp_512x512(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode5_render(scanline, width, rgb, config_ptr, length, 512, 2);
}

/* The renderer an attribute names, and the attribute a renderer came from.
 * A savestate carries the attribute: a function's address is this build's own
 * and means nothing to the build that loads the blob.
 *
 * The reverse walks the forward rather than keeping a second table, so the
 * two cannot drift apart when a renderer is added. */
vga_sprite_fn_t mode5_sprite_fn(uint16_t attributes)
{
    switch (attributes)
    {
    case 0:
        return mode5_render_1bpp_8x8;
    case 1:
        return mode5_render_2bpp_8x8;
    case 2:
        return mode5_render_4bpp_8x8;
    case 3:
        return mode5_render_8bpp_8x8;
    case 8:
        return mode5_render_1bpp_16x16;
    case 9:
        return mode5_render_2bpp_16x16;
    case 10:
        return mode5_render_4bpp_16x16;
    case 11:
        return mode5_render_8bpp_16x16;
    case 16:
        return mode5_render_1bpp_32x32;
    case 17:
        return mode5_render_2bpp_32x32;
    case 18:
        return mode5_render_4bpp_32x32;
    case 19:
        return mode5_render_8bpp_32x32;
    case 24:
        return mode5_render_1bpp_64x64;
    case 25:
        return mode5_render_2bpp_64x64;
    case 26:
        return mode5_render_4bpp_64x64;
    case 27:
        return mode5_render_8bpp_64x64;
    case 32:
        return mode5_render_1bpp_128x128;
    case 33:
        return mode5_render_2bpp_128x128;
    case 34:
        return mode5_render_4bpp_128x128;
    case 35:
        return mode5_render_8bpp_128x128;
    case 40:
        return mode5_render_1bpp_256x256;
    case 41:
        return mode5_render_2bpp_256x256;
    case 42:
        return mode5_render_4bpp_256x256;
    case 43:
        return mode5_render_8bpp_256x256;
    case 48:
        return mode5_render_1bpp_512x512;
    case 49:
        return mode5_render_2bpp_512x512;
    default:
        return NULL;
    }
}

bool mode5_sprite_attr(vga_sprite_fn_t fn, uint16_t *attributes)
{
    for (uint16_t a = 0; a < 64; a++)
        if (fn && mode5_sprite_fn(a) == fn)
        {
            *attributes = a;
            return true;
        }
    return false;
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

    const uint32_t region_size = (uint32_t)sizeof(mode5_sprite_t) * length;
    if (region_size > 0x10000 || config_ptr > 0x10000 - region_size)
        return false;

    vga_sprite_fn_t render_fn = mode5_sprite_fn(attributes);
    if (!render_fn)
        return false;

    return vga_prog_sprite(plane, scanline_begin, scanline_end, config_ptr, length, render_fn);
}

#pragma GCC pop_options
