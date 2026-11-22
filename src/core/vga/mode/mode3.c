/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/mode/mode3.h"
#include "core/vga/mode/mode.h"
#include "core/vga/vga.h"
#include "core/sys/xram.h"
#include "core/term/color.h"
#include <string.h>

#pragma GCC push_options
#pragma GCC optimize("O3")

typedef struct
{
    bool x_wrap;
    bool y_wrap;
    int16_t x_pos_px;
    int16_t y_pos_px;
    int16_t width_px;
    int16_t height_px;
    uint16_t xram_data_ptr;
    uint16_t xram_palette_ptr;
} mode3_config_t;

/* Each renderer copies the config out of XRAM once per scanline, checks the
 * copy and draws from it. A write to XRAM part way through a line, such as one
 * byte of a 16-bit size, then cannot slip in behind the checks. */

#define MODE3_HDOUBLE 0x10
#define MODE3_VDOUBLE 0x20

// The bitmap's row on this scanline, or NULL when it draws nothing. Doubled,
// the bitmap covers twice its size on the canvas. Each renderer has its own
// copy, with its depth and doubling across folded in.
static inline __attribute__((always_inline)) const uint8_t *
mode3_scanline_to_data(int16_t scanline_id, int16_t width, const mode3_config_t *config,
                       int16_t bpp, bool hdouble, bool vdouble)
{
    // At least 1 pixel each way, and a size on the canvas that fits an
    // int16_t, before the wrap divides by it.
    if (config->width_px < 1 || config->height_px < 1 ||
        ((int32_t)config->width_px << hdouble) > INT16_MAX ||
        ((int32_t)config->height_px << vdouble) > INT16_MAX)
        return NULL;
    int16_t row = scanline_id - config->y_pos_px;
    const int16_t height = (int16_t)(config->height_px << vdouble);
    if (config->y_wrap)
    {
        if (row < 0)
            row += (-(row + 1) / height + 1) * height;
        if (row >= height)
            row -= ((row - height) / height + 1) * height;
    }
    if (row < 0 || row >= height)
        return NULL;
    // A layer that misses the scanline across, like one that misses it down,
    // draws nothing. mode3_fill_cols counts on it: unwrapped, a column past
    // the right of the bitmap would leave it a negative run.
    if (!config->x_wrap &&
        (config->x_pos_px >= width || config->x_pos_px + (config->width_px << hdouble) <= 0))
        return NULL;
    const int32_t sizeof_row = ((int32_t)config->width_px * bpp + 7) / 8;
    const int32_t sizeof_bitmap = (int32_t)config->height_px * sizeof_row;
    if (sizeof_bitmap > 0x10000 - config->xram_data_ptr)
        return NULL;
    return (const uint8_t *)&xram[config->xram_data_ptr + (row >> vdouble) * sizeof_row];
}

static const uint16_t *
mode3_get_palette(mode3_config_t *config, int16_t bpp)
{
    if (!(config->xram_palette_ptr & 1) &&
        config->xram_palette_ptr <= 0x10000 - sizeof(uint16_t) * (1 << bpp))
        return (const uint16_t *)&xram[config->xram_palette_ptr];
    if (bpp == 1)
        return color_2;
    return color_256;
}

// The next run of the line inside the bitmap, which is win_w columns wide on
// the canvas, after any columns of the line left of it are cleared.
static inline __attribute__((always_inline)) int16_t
mode3_fill_cols(mode3_config_t *config, int16_t win_w, uint16_t **rgb, int16_t *col,
                int16_t *width)
{
    if (*col < 0)
    {
        if (config->x_wrap)
            *col += (-(*col + 1) / win_w + 1) * win_w;
        else
        {
            uint16_t empty_cols = -*col;
            if (empty_cols > *width)
                empty_cols = *width;
            memset(*rgb, 0, sizeof(uint16_t) * empty_cols);
            *rgb += empty_cols;
            *col += empty_cols;
            *width -= empty_cols;
            return 0;
        }
    }
    if (*col >= win_w)
    {
        if (config->x_wrap)
            *col -= ((*col - win_w) / win_w + 1) * win_w;
        else
        {
            memset(*rgb, 0, sizeof(uint16_t) * (*width));
            *width = 0;
        }
    }
    int16_t fill_cols = *width;
    if (fill_cols > win_w - *col)
        fill_cols = win_w - *col;
    *width -= fill_cols;
    return fill_cols;
}

// n pixels of the row from pixel col, which the bitmap has.
static inline __attribute__((always_inline)) void
mode3_emit(uint16_t *rgb, const uint8_t *row_data, int16_t col, int16_t n, int16_t width_px,
           const uint16_t *palette, const uint16_t *pal, const uint32_t *tbl,
           vga_fill_cache_t *cache, int16_t bpp, bool reverse)
{
    if (bpp == 16)
        memcpy(rgb, (const uint16_t *)row_data + col, sizeof(uint16_t) * n);
    else if (bpp == 8)
        mode_run_8bpp(rgb, &row_data[col], n, palette);
    else if (bpp == 4)
    {
        const uint8_t *data = &row_data[col / 2];
        if (col & 1)
        {
            *rgb++ = palette[reverse ? *data++ >> 4 : *data++ & 0xF];
            n--;
        }
        mode_run_4bpp_pairs(rgb, data, n, mode_4bpp_pairs(cache, palette, reverse));
    }
    else
    {
        // The first byte may start part way in, and the last byte of the
        // row may hold fewer pixels than it has room for.
        const int ppb = 8 / bpp;
        const uint8_t *data = &row_data[col / ppb];
        int16_t start = col & (ppb - 1);
        int16_t part = ppb - start;
        if (part > width_px - col)
            part = width_px - col;
        if (part > n)
            part = n;
        n -= part;
        if (bpp == 1 && reverse)
            mode_emit_head_1bpp_reverse(&rgb, *data++, pal, start, part);
        else if (bpp == 1)
            mode_emit_head_1bpp(&rgb, *data++, pal, start, part);
        else if (reverse)
            mode_emit_head_2bpp_reverse(&rgb, *data++, pal, start, part);
        else
            mode_emit_head_2bpp(&rgb, *data++, pal, start, part);
        const unsigned bytes = (unsigned)n / ppb;
        if (bpp == 1)
            mode_run_1bpp(rgb, data, bytes, tbl, reverse);
        else
            mode_run_2bpp(rgb, data, bytes, tbl, reverse);
        rgb += bytes * ppb;
        data += bytes;
        n -= bytes * ppb;
        if (bpp == 1 && reverse)
            mode_emit_tail_1bpp_reverse(&rgb, *data, pal, n);
        else if (bpp == 1)
            mode_emit_tail_1bpp(&rgb, *data, pal, n);
        else if (reverse)
            mode_emit_tail_2bpp_reverse(&rgb, *data, pal, n);
        else
            mode_emit_tail_2bpp(&rgb, *data, pal, n);
    }
}

// One pixel of the row, of a depth that indexes a palette.
static inline __attribute__((always_inline)) uint16_t
mode3_pixel(const uint8_t *row_data, unsigned x, const uint16_t *p, int16_t bpp, bool reverse)
{
    if (bpp == 8)
        return p[row_data[x]];
    const unsigned ppb = 8 / bpp;
    const unsigned k = x % ppb;
    const unsigned shift = reverse ? k * bpp : 8 - bpp * (k + 1);
    return p[(row_data[x / ppb] >> shift) & ((1u << bpp) - 1)];
}

// n columns of the run from column col on the canvas, every pixel of the
// bitmap drawn twice, so each whole pixel is one word. An odd column starts
// on the right half of its pixel. Below 8bpp, the pixels up to a byte are
// drawn one at a time, then whole bytes through dbl, the palette's colors
// each doubled into a word.
static inline __attribute__((always_inline)) void
mode3_emit_doubled(uint16_t *rgb, const uint8_t *row_data, unsigned col, unsigned n,
                   const uint16_t *p, const uint32_t *dbl, int16_t bpp, bool reverse)
{
    unsigned x = col >> 1;
    if (bpp == 16)
    {
        const uint8_t *src = row_data + 2 * x;
        uint16_t c;
        if (col & 1)
        {
            memcpy(&c, src, 2);
            src += 2;
            *rgb++ = c;
            n--;
        }
        for (; n >= 2; n -= 2, src += 2, rgb += 2)
        {
            memcpy(&c, src, 2);
            *(mode_word_t *)rgb = c * 0x10001u;
        }
        if (n)
        {
            memcpy(&c, src, 2);
            *rgb = c;
        }
        return;
    }
    if (col & 1)
    {
        *rgb++ = mode3_pixel(row_data, x++, p, bpp, reverse);
        n--;
    }
    mode_word_t *w = (mode_word_t *)rgb;
    if (bpp == 8)
    {
        const uint8_t *src = row_data + x;
        for (; n >= 2; n -= 2)
            *w++ = p[*src++] * 0x10001u;
        if (n)
            *(uint16_t *)w = p[*src];
        return;
    }
    const unsigned ppb = 8 / bpp;
    for (; n >= 2 && x % ppb; n -= 2)
        *w++ = mode3_pixel(row_data, x++, p, bpp, reverse) * 0x10001u;
    const uint8_t *src = row_data + x / ppb;
    const unsigned mask = (1u << bpp) - 1;
    for (; n >= 2 * ppb; n -= 2 * ppb, x += ppb)
    {
        const unsigned b = *src++;
        for (unsigned k = 0; k < ppb; k++)
            *w++ = dbl[(b >> (reverse ? k * bpp : 8 - bpp * (k + 1))) & mask];
    }
    for (; n >= 2; n -= 2)
        *w++ = mode3_pixel(row_data, x++, p, bpp, reverse) * 0x10001u;
    if (n)
        *(uint16_t *)w = mode3_pixel(row_data, x, p, bpp, reverse);
}

// One renderer for every depth. bpp, reverse and hdouble are constants at
// each call, so each wrapper below folds to one depth's byte walk.
static inline __attribute__((always_inline)) bool
mode3_render(uint16_t options, int16_t scanline_id, int16_t width, uint16_t *rgb,
             uint16_t config_ptr, vga_fill_cache_t *cache, int16_t bpp, bool reverse,
             bool hdouble)
{
    mode3_config_t cfg = *(const mode3_config_t *)&xram[config_ptr];
    mode3_config_t *config = &cfg;
    const uint8_t *row_data = mode3_scanline_to_data(scanline_id, width, config, bpp, hdouble,
                                                     options & MODE3_VDOUBLE);
    if (!row_data)
        return false;
    const int16_t win_w = (int16_t)(config->width_px << hdouble);
    const uint16_t *palette = bpp == 16 ? NULL : mode3_get_palette(config, bpp);
    MODE_TABLE(tbl, 32);
    uint16_t pal[4];
    if (bpp <= 2)
        for (int i = 0; i < (1 << bpp); i++)
            pal[i] = palette[i];
    if (hdouble)
    {
        if (bpp <= 4)
            for (int i = 0; i < (1 << bpp); i++)
                tbl[i] = (bpp <= 2 ? pal[i] : palette[i]) * 0x10001u;
    }
    else if (bpp == 1)
    {
        if (reverse)
            mode_nibble_set_reverse(tbl, pal[0], pal[1]);
        else
            mode_nibble_set(tbl, pal[0], pal[1]);
    }
    else if (bpp == 2)
    {
        if (reverse)
            mode_quad_set_reverse(tbl, pal);
        else
            mode_quad_set(tbl, pal);
    }
    int16_t col = -config->x_pos_px;
    const int16_t width_px = config->width_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, win_w, &rgb, &col, &width);
        if (fill_cols <= 0)
            continue;
        if (hdouble)
            mode3_emit_doubled(rgb, row_data, (unsigned)col, (unsigned)fill_cols,
                               bpp <= 2 ? pal : palette, tbl, bpp, reverse);
        else
            mode3_emit(rgb, row_data, col, fill_cols, width_px, palette, pal, tbl, cache, bpp,
                       reverse);
        rgb += fill_cols;
        col += fill_cols;
    }
    return true;
}

#define MODE3_RENDER_FN(name, bpp, reverse, hdouble)                           \
    static bool name(uint16_t options, int16_t scanline_id, int16_t width,     \
                     uint16_t *rgb, uint16_t config_ptr,                       \
                     vga_fill_cache_t *cache)                                  \
    {                                                                          \
        return mode3_render(options, scanline_id, width, rgb, config_ptr,      \
                            cache, bpp, reverse, hdouble);                     \
    }
MODE3_RENDER_FN(mode3_render_1bpp, 1, false, false)
MODE3_RENDER_FN(mode3_render_2bpp, 2, false, false)
MODE3_RENDER_FN(mode3_render_4bpp, 4, false, false)
MODE3_RENDER_FN(mode3_render_8bpp, 8, false, false)
MODE3_RENDER_FN(mode3_render_16bpp, 16, false, false)
MODE3_RENDER_FN(mode3_render_1bpp_reverse, 1, true, false)
MODE3_RENDER_FN(mode3_render_2bpp_reverse, 2, true, false)
MODE3_RENDER_FN(mode3_render_4bpp_reverse, 4, true, false)
MODE3_RENDER_FN(mode3_render_1bpp_hdouble, 1, false, true)
MODE3_RENDER_FN(mode3_render_2bpp_hdouble, 2, false, true)
MODE3_RENDER_FN(mode3_render_4bpp_hdouble, 4, false, true)
MODE3_RENDER_FN(mode3_render_8bpp_hdouble, 8, false, true)
MODE3_RENDER_FN(mode3_render_16bpp_hdouble, 16, false, true)
MODE3_RENDER_FN(mode3_render_1bpp_reverse_hdouble, 1, true, true)
MODE3_RENDER_FN(mode3_render_2bpp_reverse_hdouble, 2, true, true)
MODE3_RENDER_FN(mode3_render_4bpp_reverse_hdouble, 4, true, true)
#undef MODE3_RENDER_FN

/* Every depth this mode defines and the renderers it names, undoubled and
 * doubled across, are written here once. mode3_fill_fn and mode3_fill_valid
 * both expand this list, so neither can drift from the other. Doubling down
 * goes with any of them.
 *
 * A fabric build expands only the attribute column. Nothing there calls these
 * renderers, and that image has one 96 KB memory for text, stack and heap. */
#define MODE3_FILLS(F)                                                      \
    F(0, mode3_render_1bpp, mode3_render_1bpp_hdouble)                      \
    F(1, mode3_render_2bpp, mode3_render_2bpp_hdouble)                      \
    F(2, mode3_render_4bpp, mode3_render_4bpp_hdouble)                      \
    F(3, mode3_render_8bpp, mode3_render_8bpp_hdouble)                      \
    F(4, mode3_render_16bpp, mode3_render_16bpp_hdouble)                    \
    F(8, mode3_render_1bpp_reverse, mode3_render_1bpp_reverse_hdouble)      \
    F(9, mode3_render_2bpp_reverse, mode3_render_2bpp_reverse_hdouble)      \
    F(10, mode3_render_4bpp_reverse, mode3_render_4bpp_reverse_hdouble)

bool mode3_fill_valid(uint16_t attributes)
{
    switch (attributes & ~(MODE3_HDOUBLE | MODE3_VDOUBLE))
    {
#define MODE3_CASE(attr, fn, fn_hdouble) case attr:
        MODE3_FILLS(MODE3_CASE)
#undef MODE3_CASE
        return true;
    default:
        return false;
    }
}

#ifndef RP6502_VGA_FABRIC
vga_fill_fn_t mode3_fill_fn(uint16_t attributes)
{
    switch (attributes & ~(MODE3_HDOUBLE | MODE3_VDOUBLE))
    {
#define MODE3_CASE(attr, fn, fn_hdouble) \
    case attr:                            \
        return attributes & MODE3_HDOUBLE ? fn_hdouble : fn;
        MODE3_FILLS(MODE3_CASE)
#undef MODE3_CASE
    default:
        return NULL;
    }
}
#endif

bool mode3_config_ok(uint16_t config_ptr)
{
    return vga_xram_ok(config_ptr, sizeof(mode3_config_t));
}

bool mode3_prog(uint16_t *xregs)
{
    const uint16_t attributes = xregs[2];
    const uint16_t config_ptr = xregs[3];
    const int16_t plane = xregs[4];
    const int16_t scanline_begin = xregs[5];
    const int16_t scanline_end = xregs[6];

    if (!mode3_config_ok(config_ptr))
        return false;

    if (!mode3_fill_valid(attributes))
        return false;

    return vga_prog_fill(plane, scanline_begin, scanline_end, config_ptr, 3, attributes);
}

#pragma GCC pop_options
