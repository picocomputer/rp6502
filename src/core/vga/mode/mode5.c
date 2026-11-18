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

// Pixel j of the bytes at src, leftmost pixel in the most significant bits.
static inline __attribute__((always_inline)) uint16_t
mode5_pixel(const uint8_t *src, unsigned j, const uint16_t *palette, int bpp)
{
    if (bpp == 8)
        return palette[src[j]];
    const unsigned ppb = 8 / bpp;
    const unsigned shift = 8 - bpp * (j % ppb + 1);
    return palette[(src[j / ppb] >> shift) & ((1u << bpp) - 1)];
}

/* Eight columns from whole bytes at src. Their colors are looked up
 * together, and when all eight are opaque, which is most of a sprite, they
 * are stored as whole words without a test per pixel. The flips and doubling
 * only change where the words land: dst is the first column's pixel, as in
 * mode5_blit_body. */
static inline __attribute__((always_inline)) void
mode5_group(uint16_t *dst, const uint8_t *src, const uint16_t *palette,
            bool hflip, bool hdouble, int bpp)
{
    uint16_t c[8];
    if (bpp == 8)
    {
        /* The eight bytes as two words, unpacked in registers. */
        const uint32_t a = mode_load32(src), b = mode_load32(src + 4);
        for (unsigned k = 0; k < 4; k++)
        {
            c[k] = palette[(a >> (8 * k)) & 0xFF];
            c[k + 4] = palette[(b >> (8 * k)) & 0xFF];
        }
    }
    else if (bpp == 4)
    {
        /* The four bytes as one word, the high nibble of each first. */
        const uint32_t a = mode_load32(src);
        for (unsigned k = 0; k < 4; k++)
        {
            c[2 * k] = palette[(a >> (8 * k + 4)) & 0xF];
            c[2 * k + 1] = palette[(a >> (8 * k)) & 0xF];
        }
    }
    else
        for (unsigned k = 0; k < 8; k++)
            c[k] = mode5_pixel(src, k, palette, bpp);
    const uint32_t opaque = 0x00200020;
    if (!hdouble)
    {
        uint32_t w[4];
        for (unsigned k = 0; k < 4; k++)
            w[k] = hflip ? mode_pack2(c[7 - 2 * k], c[6 - 2 * k])
                         : mode_pack2(c[2 * k], c[2 * k + 1]);
        mode_word_t *d = (mode_word_t *)(hflip ? dst - 7 : dst);
        if ((w[0] & w[1] & w[2] & w[3] & opaque) == opaque)
            for (unsigned k = 0; k < 4; k++)
                d[k] = w[k];
        else
            for (unsigned k = 0; k < 4; k++)
                d[k] = mode_alpha_merge(w[k], d[k]);
    }
    else
    {
        mode_word_t *d = (mode_word_t *)(hflip ? dst - 15 : dst);
        for (unsigned k = 0; k < 8; k++)
        {
            const uint16_t color = c[hflip ? 7 - k : k];
            if (color & (1 << 5))
                d[k] = color * 0x10001u;
        }
    }
}

/* Paints image columns s_lo through s_lo + count - 1 in that order, so a flip
 * is the direction dst steps. dst is the canvas pixel the first column lands
 * on; doubled, it is the left of that column's pair, or the right when
 * flipped. lead and tail mark a doubled column with one pixel off the canvas
 * at the first and the last column.
 *
 * The middle of the run goes eight columns at a time through mode5_group. */
static inline __attribute__((always_inline)) void
mode5_blit_body(uint16_t *dst, const uint8_t *row, const uint16_t *palette,
                unsigned col, unsigned count, bool lead, bool tail,
                bool hflip, bool hdouble, int bpp)
{
    const int step = hflip ? -1 : 1;
    const unsigned ppb = 8 / bpp;
    if (lead)
    {
        const uint16_t color = mode5_pixel(row, col++, palette, bpp);
        if (color & (1 << 5))
            *dst = color;
        dst += step;
        count--;
    }
    if (tail)
        count--;
    /* One column, for the columns that do not fill a group of eight. */
#define MODE5_ONE()                                                            \
    do                                                                         \
    {                                                                          \
        const uint16_t color = mode5_pixel(row, col++, palette, bpp);          \
        if (hdouble)                                                           \
        {                                                                      \
            if (color & (1 << 5))                                              \
                *(mode_word_t *)(hflip ? dst - 1 : dst) = mode_pack2(color, color); \
            dst += 2 * step;                                                   \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            if (color & (1 << 5))                                              \
                *dst = color;                                                  \
            dst += step;                                                       \
        }                                                                      \
        count--;                                                               \
    } while (0)
    while (count && col % ppb)
        MODE5_ONE();
    const uint8_t *src = row + col / ppb;
    for (; count >= 8; count -= 8, col += 8, src += bpp)
    {
        mode5_group(dst, src, palette, hflip, hdouble, bpp);
        dst += (hdouble ? 16 : 8) * step;
    }
    while (count)
        MODE5_ONE();
#undef MODE5_ONE
    if (tail)
    {
        const uint16_t color = mode5_pixel(row, col, palette, bpp);
        if (color & (1 << 5))
            *dst = color;
    }
}

/* Two copies of the blit per depth: the plain one, which every sprite size
 * and most custom sprites draw with, and one that takes the flips and
 * doubling at run time. Unswitching that one would make a copy for every
 * combination. */
/* Only GCC takes optimize as a function attribute. */
#if defined(__GNUC__) && !defined(__clang__)
#define MODE5_NO_UNSWITCH __attribute__((optimize("no-unswitch-loops")))
#else
#define MODE5_NO_UNSWITCH
#endif
#define MODE5_BLIT_FN(bpp)                                                       \
    static void __attribute__((noinline))                                        \
    mode5_plain_##bpp(uint16_t *dst, const uint8_t *row,                         \
                      const uint16_t *palette, unsigned col, unsigned count)     \
    {                                                                            \
        mode5_blit_body(dst, row, palette, col, count, false, false,             \
                        false, false, bpp);                                      \
    }                                                                            \
    static void __attribute__((noinline)) MODE5_NO_UNSWITCH                     \
    mode5_blit_##bpp(uint16_t *dst, const uint8_t *row, const uint16_t *palette, \
                     unsigned col, unsigned count, bool lead, bool tail,         \
                     bool hflip, bool hdouble)                                   \
    {                                                                            \
        mode5_blit_body(dst, row, palette, col, count, lead, tail,               \
                        hflip, hdouble, bpp);                                    \
    }
MODE5_BLIT_FN(1)
MODE5_BLIT_FN(2)
MODE5_BLIT_FN(4)
MODE5_BLIT_FN(8)
#undef MODE5_BLIT_FN
#undef MODE5_NO_UNSWITCH

static inline __attribute__((always_inline)) void
mode5_blit(uint16_t *dst, const uint8_t *row, const uint16_t *palette,
           unsigned s_lo, unsigned count, bool lead, bool tail,
           int bpp, bool hflip, bool hdouble)
{
    if (!hflip && !hdouble)
        switch (bpp)
        {
        case 1: mode5_plain_1(dst, row, palette, s_lo, count); break;
        case 2: mode5_plain_2(dst, row, palette, s_lo, count); break;
        case 4: mode5_plain_4(dst, row, palette, s_lo, count); break;
        default: mode5_plain_8(dst, row, palette, s_lo, count); break;
        }
    else
        switch (bpp)
        {
        case 1: mode5_blit_1(dst, row, palette, s_lo, count, lead, tail, hflip, hdouble); break;
        case 2: mode5_blit_2(dst, row, palette, s_lo, count, lead, tail, hflip, hdouble); break;
        case 4: mode5_blit_4(dst, row, palette, s_lo, count, lead, tail, hflip, hdouble); break;
        default: mode5_blit_8(dst, row, palette, s_lo, count, lead, tail, hflip, hdouble); break;
        }
}

/* With unroll, sprite_size is a constant, and a sprite wholly on the line
 * is drawn by a copy of the blit fixed to that size, unrolled whole. */
static inline __attribute__((always_inline)) void
mode5_render(int16_t scanline, int16_t width, uint16_t *rgb,
             uint16_t config_ptr, uint16_t length,
             int sprite_size, int bpp, bool unroll)
{
    const int16_t bytes_per_row = sprite_size * bpp / 8;
    const uint32_t sprite_data_size = (uint32_t)sprite_size * bytes_per_row;
    if (sprite_data_size > 0x10000)
        return;

    const mode5_sprite_t *sprites = (const mode5_sprite_t *)&xram[config_ptr];

    for (const mode5_sprite_t *s = sprites, *end = sprites + length; s < end; s++)
    {
        /* Each field is read out of XRAM once, checked, and drawn from that
         * copy, so a write part way through the line cannot slip in behind
         * the checks. Most sprites miss the line, so the row is read and
         * tested before anything else. */
        const int tex_y = scanline - s->y_pos_px;
        if ((unsigned)tex_y >= (unsigned)sprite_size)
            continue;
        const mode5_sprite_t sp = *s;

        if (unroll && sp.x_pos_px >= 0 && sp.x_pos_px <= width - sprite_size)
        {
            if (sp.xram_sprite_ptr > 0x10000 - sprite_data_size)
                continue;
            uint16_t *dst = rgb + sp.x_pos_px;
            const uint8_t *src = (const uint8_t *)&xram[sp.xram_sprite_ptr + tex_y * bytes_per_row];
            const uint16_t *palette = mode5_get_palette(sp.palette_ptr, bpp);
#pragma GCC unroll 8
            for (int g = 0; g < sprite_size / 8; g++)
                mode5_group(dst + 8 * g, src + g * bpp, palette, false, false, bpp);
            continue;
        }

        int16_t x_start = sp.x_pos_px;
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

        if (sp.xram_sprite_ptr > 0x10000 - sprite_data_size)
            continue;

        const uint16_t *palette = mode5_get_palette(sp.palette_ptr, bpp);
        const uint8_t *row_data =
            (const uint8_t *)&xram[sp.xram_sprite_ptr + tex_y * bytes_per_row];
        mode5_blit(rgb + x_start, row_data, palette, tex_x, size_x,
                   false, false, bpp, false, false);
    }
}

/* One custom sprite on the line, from the fields already read: its row of
 * the texture, tex_y, and its size and options bytes. bpp, the flip and the
 * doubling across are constants, so each combination is its own copy. */
static inline __attribute__((always_inline)) void
mode5_custom_sprite(const mode5_csprite_t *s, int tex_y, int width_height, int options,
                    int16_t width, uint16_t *rgb, int bpp, bool hflip, bool hdouble)
{
    const mode5_csprite_t sp = {
        .x_pos_px = s->x_pos_px,
        .xram_sprite_ptr = s->xram_sprite_ptr,
        .palette_ptr = s->palette_ptr,
    };
    const int nw = width_height & 15;
    const int w = (nw + 1) * 4;
    const int h = ((width_height >> 4) + 1) * 4;
    const int x = sp.x_pos_px;
    const int footprint = w << hdouble;
    if (x + footprint <= 0 || x >= width)
        return;
    const int bytes_per_row = (w * bpp + 7) / 8;
    if (sp.xram_sprite_ptr > 0x10000 - (unsigned)(h * bytes_per_row))
        return;
    int row = tex_y >> (options >> 7);
    if (options & MODE5_VFLIP)
        row = h - 1 - row;
    const uint8_t *src = (const uint8_t *)&xram[sp.xram_sprite_ptr + row * bytes_per_row];
    const uint16_t *palette = mode5_get_palette(sp.palette_ptr, bpp);

    /* Wholly on the line, the usual case, the blit is inlined here. */
    if (x >= 0 && x <= width - footprint)
    {
        mode5_blit_body(rgb + (hflip ? x + footprint - 1 : x), src, palette,
                        0, w, false, false, hflip, hdouble, bpp);
        return;
    }

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
    mode5_blit(dst, src, palette, s_lo, s_hi - s_lo + 1, lead, tail,
               bpp, hflip, hdouble);
}

static void
mode5_render_custom(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    const mode5_csprite_t *sprites = (const mode5_csprite_t *)&xram[config_ptr];

    for (const mode5_csprite_t *s = sprites, *end = sprites + length; s < end; s++)
    {
        /* Each field is read out of XRAM once, checked, and drawn from that
         * copy, so a write part way through the line cannot slip in behind
         * the checks. Most sprites miss the line, so the three fields that
         * place it down the canvas are read and tested first. */
        const int tex_y = scanline - s->y_pos_px;
        const int width_height = s->width_height;
        const int options = s->options;
        const int h = ((width_height >> 4) + 1) * 4;
        if ((unsigned)tex_y >= (unsigned)(h << (options >> 7)))
            continue;
        switch (options & (MODE5_HDOUBLE | MODE5_HFLIP | 3))
        {
#define MODE5_CUSTOM_CASE(log, hf, hd)                                               \
    case (hd ? MODE5_HDOUBLE : 0) | (hf ? MODE5_HFLIP : 0) | log:                    \
        mode5_custom_sprite(s, tex_y, width_height, options, width, rgb, 1 << log, hf, hd); \
        break;
            MODE5_CUSTOM_CASE(0, 0, 0) MODE5_CUSTOM_CASE(0, 0, 1) MODE5_CUSTOM_CASE(0, 1, 0) MODE5_CUSTOM_CASE(0, 1, 1)
            MODE5_CUSTOM_CASE(1, 0, 0) MODE5_CUSTOM_CASE(1, 0, 1) MODE5_CUSTOM_CASE(1, 1, 0) MODE5_CUSTOM_CASE(1, 1, 1)
            MODE5_CUSTOM_CASE(2, 0, 0) MODE5_CUSTOM_CASE(2, 0, 1) MODE5_CUSTOM_CASE(2, 1, 0) MODE5_CUSTOM_CASE(2, 1, 1)
            MODE5_CUSTOM_CASE(3, 0, 0) MODE5_CUSTOM_CASE(3, 0, 1) MODE5_CUSTOM_CASE(3, 1, 0) MODE5_CUSTOM_CASE(3, 1, 1)
#undef MODE5_CUSTOM_CASE
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

/* For the larger sizes the size only scales the arithmetic, so one renderer
 * per depth takes it as an argument. */
#define MODE5_DEPTH_FN(bpp)                                                     \
    static void __attribute__((noinline))                                       \
    mode5_render_depth_##bpp(int16_t scanline, int16_t width, uint16_t *rgb,     \
                            uint16_t config_ptr, uint16_t length, int size)     \
    {                                                                           \
        mode5_render(scanline, width, rgb, config_ptr, length, size, bpp, false); \
    }
MODE5_DEPTH_FN(1)
MODE5_DEPTH_FN(2)
MODE5_DEPTH_FN(4)
MODE5_DEPTH_FN(8)
#undef MODE5_DEPTH_FN

/* Up to 64 wide, each size and depth has its own renderer with the blit
 * unrolled; the larger sizes share one per depth. */
#define MODE5_FIXED_FN(attr, size, bpp)                                            \
    static void                                                                    \
    MODE5_FIXED_NAME(size, bpp)(int16_t scanline, int16_t width, uint16_t *rgb,    \
                                uint16_t config_ptr, uint16_t length)              \
    {                                                                              \
        if (size <= 64)                                                            \
            mode5_render(scanline, width, rgb, config_ptr, length, size, bpp, true); \
        else                                                                       \
            mode5_render_depth_##bpp(scanline, width, rgb, config_ptr, length, size); \
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
