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

        // Walking the source byte in a register costs one fetch per byte.
        // Deriving each pixel's address from its column instead costs a shift
        // and, because the column is signed, a division fixup as well.
        if (bpp == 8)
        {
            const uint8_t *src = row_data + (unsigned)tex_x;
            for (int16_t n = size_x; n; n--, dst++)
            {
                const uint16_t color = palette[*src++];
                if (color & (1 << 5))
                    *dst = color;
            }
        }
        else
        {
            const unsigned ppb = 8 / bpp;
            const unsigned phase = (unsigned)tex_x % ppb;
            const uint8_t *src = row_data + (unsigned)tex_x / ppb;
            unsigned left = ppb - phase;
            uint8_t bits = (uint8_t)(*src++ << (phase * bpp));
            for (int16_t n = size_x; n; n--, dst++)
            {
                if (left == 0)
                {
                    bits = *src++;
                    left = ppb;
                }
                const uint16_t color = palette[bits >> (8 - bpp)];
                if (color & (1 << 5))
                    *dst = color;
                bits = (uint8_t)(bits << bpp);
                left--;
            }
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

/* Every attribute this mode defines and the renderer it names is written here
 * once. mode5_sprite_fn and mode5_sprite_valid both expand this list, so
 * neither can drift from the other.
 *
 * A fabric build expands only the attribute column. Nothing there calls these
 * renderers, and that image has one 96 KB memory for text, stack and heap. */
#define MODE5_SPRITES(F) \
    F(0, mode5_render_1bpp_8x8)      \
    F(1, mode5_render_2bpp_8x8)      \
    F(2, mode5_render_4bpp_8x8)      \
    F(3, mode5_render_8bpp_8x8)      \
    F(8, mode5_render_1bpp_16x16)    \
    F(9, mode5_render_2bpp_16x16)    \
    F(10, mode5_render_4bpp_16x16)   \
    F(11, mode5_render_8bpp_16x16)   \
    F(16, mode5_render_1bpp_32x32)   \
    F(17, mode5_render_2bpp_32x32)   \
    F(18, mode5_render_4bpp_32x32)   \
    F(19, mode5_render_8bpp_32x32)   \
    F(24, mode5_render_1bpp_64x64)   \
    F(25, mode5_render_2bpp_64x64)   \
    F(26, mode5_render_4bpp_64x64)   \
    F(27, mode5_render_8bpp_64x64)   \
    F(32, mode5_render_1bpp_128x128) \
    F(33, mode5_render_2bpp_128x128) \
    F(34, mode5_render_4bpp_128x128) \
    F(35, mode5_render_8bpp_128x128) \
    F(40, mode5_render_1bpp_256x256) \
    F(41, mode5_render_2bpp_256x256) \
    F(42, mode5_render_4bpp_256x256) \
    F(43, mode5_render_8bpp_256x256) \
    F(48, mode5_render_1bpp_512x512) \
    F(49, mode5_render_2bpp_512x512)

bool mode5_sprite_valid(uint16_t attributes)
{
    switch (attributes)
    {
#define MODE5_CASE(attr, fn) case attr:
        MODE5_SPRITES(MODE5_CASE)
#undef MODE5_CASE
        return true;
    default:
        return false;
    }
}

#ifdef RP6502_VGA_FABRIC

vga_sprite_fn_t mode5_sprite_fn(uint16_t attributes)
{
    (void)attributes;
    return NULL;
}

#else

vga_sprite_fn_t mode5_sprite_fn(uint16_t attributes)
{
    switch (attributes)
    {
#define MODE5_CASE(attr, fn) \
    case attr:                \
        return fn;
        MODE5_SPRITES(MODE5_CASE)
#undef MODE5_CASE
    default:
        return NULL;
    }
}

#endif

#ifndef RP6502_VGA_FABRIC
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
#endif

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

    if (!mode5_sprite_valid(attributes))
        return false;
    vga_sprite_fn_t render_fn = mode5_sprite_fn(attributes);

    return vga_prog_sprite(plane, scanline_begin, scanline_end, config_ptr, length, render_fn);
}

#pragma GCC pop_options
