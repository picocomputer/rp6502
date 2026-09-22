/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/mode/mode5.h"
#include "core/vga/mode/mode.h"
#include "core/sys/xram.h"
#include "core/vga/vga.h"
#include "core/term/color.h"

#pragma GCC push_options
#pragma GCC optimize("O3")

#define MODE5_CUSTOM 0x38
#define MODE5_HFLIP 0x10
#define MODE5_VFLIP 0x20
#define MODE5_HDOUBLE 0x40
#define MODE5_VDOUBLE 0x80

typedef struct
{
    int16_t x_pos_px;
    int16_t y_pos_px;
    uint16_t xram_sprite_ptr;
    uint16_t palette_ptr;
} mode5_sprite_t;

typedef struct
{
    int16_t x_pos_px;
    int16_t y_pos_px;
    uint16_t xram_sprite_ptr;
    uint16_t palette_ptr;
    uint8_t width_height;
    uint8_t options;
} mode5_csprite_t;

_Static_assert(sizeof(mode5_csprite_t) == 10, "a custom descriptor is ten bytes");
_Static_assert(offsetof(mode5_csprite_t, width_height) == 8 &&
                   offsetof(mode5_csprite_t, options) == 9,
               "the size and options bytes follow the palette pointer");

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

// Walking the source byte in a register costs one fetch per byte.
// Deriving each pixel's address from its column instead costs a shift
// and, because the column is signed, a division fixup as well.
typedef struct
{
    const uint8_t *src;
    unsigned bits;
    unsigned left;
} mode5_src_t;

static inline __attribute__((always_inline)) void
mode5_src_seek(mode5_src_t *s, const uint8_t *row, unsigned col, int bpp)
{
    if (bpp == 8)
    {
        s->src = row + col;
        s->bits = 0;
        s->left = 0;
        return;
    }
    const unsigned ppb = 8 / bpp;
    const unsigned phase = col % ppb;
    s->src = row + col / ppb;
    s->left = ppb - phase;
    s->bits = (uint8_t)(*s->src++ << (phase * bpp));
}

static inline __attribute__((always_inline)) uint16_t
mode5_src_next(mode5_src_t *s, const uint16_t *palette, int bpp)
{
    if (bpp == 8)
        return palette[*s->src++];
    if (s->left == 0)
    {
        s->bits = *s->src++;
        s->left = 8 / bpp;
    }
    const uint16_t color = palette[s->bits >> (8 - bpp)];
    s->bits = (uint8_t)(s->bits << bpp);
    s->left--;
    return color;
}

/* Paints image columns s_lo through s_lo + count - 1 in that order, so a flip
 * is the direction dst steps. dst is the canvas pixel the first column lands
 * on; doubled, it is the left of that column's pair, or the right when
 * flipped. lead and tail mark a doubled column with one pixel off the canvas
 * at the first and the last column. */
static inline __attribute__((always_inline)) void
mode5_blit(uint16_t *dst, const uint8_t *row, const uint16_t *palette,
           unsigned s_lo, unsigned count, bool lead, bool tail,
           int bpp, bool hflip, bool hdouble)
{
    mode5_src_t s;
    mode5_src_seek(&s, row, s_lo, bpp);
    if (!hdouble)
    {
        for (unsigned n = count; n; n--)
        {
            const uint16_t color = mode5_src_next(&s, palette, bpp);
            if (color & (1 << 5))
                *dst = color;
            dst += hflip ? -1 : 1;
        }
        return;
    }
    unsigned pairs = count;
    if (lead)
    {
        const uint16_t color = mode5_src_next(&s, palette, bpp);
        if (color & (1 << 5))
            *dst = color;
        dst += hflip ? -1 : 1;
        pairs--;
    }
    if (tail)
        pairs--;
    for (; pairs; pairs--)
    {
        const uint16_t color = mode5_src_next(&s, palette, bpp);
        if (color & (1 << 5))
            *(mode_word_t *)(hflip ? dst - 1 : dst) = mode_pack2(color, color);
        dst += hflip ? -2 : 2;
    }
    if (tail)
    {
        const uint16_t color = mode5_src_next(&s, palette, bpp);
        if (color & (1 << 5))
            *dst = color;
    }
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
        mode5_blit(rgb + x_start, row_data, palette, tex_x, size_x,
                   false, false, bpp, false, false);
    }
}

#define MODE5_BLITS(F) \
    F(0, 1, false, false) \
    F(1, 2, false, false) \
    F(2, 4, false, false) \
    F(3, 8, false, false) \
    F(4, 1, true, false)  \
    F(5, 2, true, false)  \
    F(6, 4, true, false)  \
    F(7, 8, true, false)  \
    F(8, 1, false, true)  \
    F(9, 2, false, true)  \
    F(10, 4, false, true) \
    F(11, 8, false, true) \
    F(12, 1, true, true)  \
    F(13, 2, true, true)  \
    F(14, 4, true, true)  \
    F(15, 8, true, true)

static void
mode5_render_custom(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    const mode5_csprite_t *sprites = (const mode5_csprite_t *)&xram[config_ptr];

    for (uint16_t i = 0; i < length; i++)
    {
        const int options = sprites[i].options;
        const int bpp_log = options & 3;
        const int hflip = (options & MODE5_HFLIP) != 0;
        const int vflip = (options & MODE5_VFLIP) != 0;
        const int hdouble = (options & MODE5_HDOUBLE) != 0;
        const int vdouble = (options & MODE5_VDOUBLE) != 0;
        const int nw = sprites[i].width_height & 15;
        const int w = (nw + 1) * 4;
        const int h = ((sprites[i].width_height >> 4) + 1) * 4;

        const int tex_y = scanline - sprites[i].y_pos_px;
        if (tex_y < 0 || tex_y >= (h << vdouble))
            continue;
        int row = tex_y >> vdouble;
        if (vflip)
            row = h - 1 - row;

        const int x = sprites[i].x_pos_px;
        const int footprint = w << hdouble;
        if (x + footprint <= 0 || x >= width)
            continue;

        const int bytes_per_row = (((nw + 1) << bpp_log) + 1) >> 1;
        const unsigned sprite_data_size = h * bytes_per_row;
        if (sprites[i].xram_sprite_ptr > 0x10000 - sprite_data_size)
            continue;

        const int lo = x < 0 ? -x : 0;
        const int hi = x + footprint > width ? x + footprint - width : 0;
        const int a = hflip ? hi : lo;
        const int b = hflip ? lo : hi;
        const int s_lo = a >> hdouble;
        const int s_hi = w - 1 - (b >> hdouble);
        const int right = x + footprint < width ? x + footprint : width;
        uint16_t *dst = rgb + (hflip ? right - 1 : x > 0 ? x : 0);
        const bool lead = hdouble && (a & 1);
        const bool tail = hdouble && (b & 1);

        const uint16_t *palette = mode5_get_palette(sprites[i].palette_ptr, 1 << bpp_log);
        const uint8_t *row_data =
            (const uint8_t *)&xram[sprites[i].xram_sprite_ptr + row * bytes_per_row];

        switch (bpp_log | (hflip << 2) | (hdouble << 3))
        {
#define MODE5_BLIT_CASE(key, bpp, hf, hd)                            \
    case key:                                                        \
        mode5_blit(dst, row_data, palette, s_lo, s_hi - s_lo + 1,    \
                   lead, tail, bpp, hf, hd);                         \
        break;
            MODE5_BLITS(MODE5_BLIT_CASE)
#undef MODE5_BLIT_CASE
        }
    }
}

/* Every attribute this mode defines and the renderer it names is written here
 * once: a fixed row as (attribute, size, bpp), the custom row as (attribute).
 * The fixed wrappers, mode5_sprite_fn and mode5_sprite_valid all expand this
 * list, so none can drift from the others.
 *
 * A fabric build expands only the attribute column. Nothing there calls these
 * renderers, and that image has one 96 KB memory for text, stack and heap. */
#define MODE5_SPRITES(FIXED, CUSTOM) \
    FIXED(0, 8, 1)                   \
    FIXED(1, 8, 2)                   \
    FIXED(2, 8, 4)                   \
    FIXED(3, 8, 8)                   \
    FIXED(8, 16, 1)                  \
    FIXED(9, 16, 2)                  \
    FIXED(10, 16, 4)                 \
    FIXED(11, 16, 8)                 \
    FIXED(16, 32, 1)                 \
    FIXED(17, 32, 2)                 \
    FIXED(18, 32, 4)                 \
    FIXED(19, 32, 8)                 \
    FIXED(24, 64, 1)                 \
    FIXED(25, 64, 2)                 \
    FIXED(26, 64, 4)                 \
    FIXED(27, 64, 8)                 \
    FIXED(32, 128, 1)                \
    FIXED(33, 128, 2)                \
    FIXED(34, 128, 4)                \
    FIXED(35, 128, 8)                \
    FIXED(40, 256, 1)                \
    FIXED(41, 256, 2)                \
    FIXED(42, 256, 4)                \
    FIXED(43, 256, 8)                \
    FIXED(48, 512, 1)                \
    FIXED(49, 512, 2)                \
    CUSTOM(MODE5_CUSTOM)

#define MODE5_FIXED_NAME(size, bpp) mode5_render_##bpp##bpp_##size##x##size

#define MODE5_FIXED_FN(attr, size, bpp)                                            \
    static void                                                                    \
    MODE5_FIXED_NAME(size, bpp)(int16_t scanline, int16_t width, uint16_t *rgb,    \
                                uint16_t config_ptr, uint16_t length)              \
    {                                                                              \
        mode5_render(scanline, width, rgb, config_ptr, length, size, bpp);         \
    }
#define MODE5_CUSTOM_FN(attr)
MODE5_SPRITES(MODE5_FIXED_FN, MODE5_CUSTOM_FN)
#undef MODE5_FIXED_FN
#undef MODE5_CUSTOM_FN

/* A custom sprite's depth is in its descriptor, so the attribute's depth bits
 * mean nothing and every attribute 0x38..0x3F names the custom renderer. */
static inline uint16_t mode5_sprite_key(uint16_t attributes)
{
    if ((attributes & ~7) == MODE5_CUSTOM)
        return MODE5_CUSTOM;
    return attributes;
}

bool mode5_sprite_valid(uint16_t attributes)
{
    switch (mode5_sprite_key(attributes))
    {
#define MODE5_FIXED_CASE(attr, size, bpp) case attr:
#define MODE5_CUSTOM_CASE(attr) case attr:
        MODE5_SPRITES(MODE5_FIXED_CASE, MODE5_CUSTOM_CASE)
#undef MODE5_FIXED_CASE
#undef MODE5_CUSTOM_CASE
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
    switch (mode5_sprite_key(attributes))
    {
#define MODE5_FIXED_CASE(attr, size, bpp) \
    case attr:                            \
        return MODE5_FIXED_NAME(size, bpp);
#define MODE5_CUSTOM_CASE(attr) \
    case attr:                  \
        return mode5_render_custom;
        MODE5_SPRITES(MODE5_FIXED_CASE, MODE5_CUSTOM_CASE)
#undef MODE5_FIXED_CASE
#undef MODE5_CUSTOM_CASE
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

    const uint32_t descriptor_size = mode5_sprite_key(attributes) == MODE5_CUSTOM
                                         ? sizeof(mode5_csprite_t)
                                         : sizeof(mode5_sprite_t);
    const uint32_t region_size = descriptor_size * length;
    if (region_size > 0x10000 || config_ptr > 0x10000 - region_size)
        return false;

    if (!mode5_sprite_valid(attributes))
        return false;
    vga_sprite_fn_t render_fn = mode5_sprite_fn(attributes);

    return vga_prog_sprite(plane, scanline_begin, scanline_end, config_ptr, length, render_fn);
}

#pragma GCC pop_options
