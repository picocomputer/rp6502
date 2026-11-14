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

static const uint8_t *
mode3_scanline_to_data(int16_t scanline_id, int16_t width, mode3_config_t *config, int16_t bpp)
{
    // At least 1 pixel each way, before the wrap divides by the height.
    if (config->width_px < 1 || config->height_px < 1)
        return NULL;
    int16_t row = scanline_id - config->y_pos_px;
    const int16_t height = config->height_px;
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
        (config->x_pos_px >= width || config->x_pos_px + config->width_px <= 0))
        return NULL;
    const int32_t sizeof_row = ((int32_t)config->width_px * bpp + 7) / 8;
    const int32_t sizeof_bitmap = (int32_t)height * sizeof_row;
    if (sizeof_bitmap > 0x10000 - config->xram_data_ptr)
        return NULL;
    return (const uint8_t *)&xram[config->xram_data_ptr + row * sizeof_row];
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

static inline __attribute__((always_inline)) int16_t
mode3_fill_cols(mode3_config_t *config, uint16_t **rgb, int16_t *col, int16_t *width)
{
    if (*col < 0)
    {
        if (config->x_wrap)
            *col += (-(*col + 1) / config->width_px + 1) * config->width_px;
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
    if (*col >= config->width_px)
    {
        if (config->x_wrap)
            *col -= ((*col - config->width_px) / config->width_px + 1) * config->width_px;
        else
        {
            memset(*rgb, 0, sizeof(uint16_t) * (*width));
            *width = 0;
        }
    }
    int16_t fill_cols = *width;
    if (fill_cols > config->width_px - *col)
        fill_cols = config->width_px - *col;
    *width -= fill_cols;
    return fill_cols;
}

// One renderer for every depth. bpp and reverse are constants at each call, so
// each wrapper below folds to one depth's byte walk.
static inline __attribute__((always_inline)) bool
mode3_render(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr,
             vga_fill_cache_t *cache, int16_t bpp, bool reverse)
{
    mode3_config_t cfg = *(const mode3_config_t *)&xram[config_ptr];
    mode3_config_t *config = &cfg;
    const uint8_t *row_data = mode3_scanline_to_data(scanline_id, width, config, bpp);
    if (!row_data)
        return false;
    const uint16_t *palette = bpp == 16 ? NULL : mode3_get_palette(config, bpp);
    MODE_TABLE(tbl, 32);
    uint16_t pal[4];
    if (bpp == 1)
    {
        pal[0] = palette[0];
        pal[1] = palette[1];
        if (reverse)
            mode_nibble_set_reverse(tbl, pal[0], pal[1]);
        else
            mode_nibble_set(tbl, pal[0], pal[1]);
    }
    else if (bpp == 2)
    {
        for (int i = 0; i < 4; i++)
            pal[i] = palette[i];
        if (reverse)
            mode_quad_set_reverse(tbl, pal);
        else
            mode_quad_set(tbl, pal);
    }
    int16_t col = -config->x_pos_px;
    const int16_t width_px = config->width_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        if (fill_cols <= 0)
            continue;
        if (bpp == 16)
        {
            memcpy(rgb, (const uint16_t *)row_data + col, sizeof(uint16_t) * fill_cols);
            rgb += fill_cols;
            col += fill_cols;
        }
        else if (bpp == 8)
        {
            mode_run_8bpp(rgb, &row_data[col], fill_cols, palette);
            rgb += fill_cols;
            col += fill_cols;
        }
        else if (bpp == 4)
        {
            const uint8_t *data = &row_data[col / 2];
            if (col & 1)
            {
                *rgb++ = palette[reverse ? *data++ >> 4 : *data++ & 0xF];
                col++;
                fill_cols--;
            }
            mode_run_4bpp_pairs(rgb, data, fill_cols, mode_4bpp_pairs(cache, palette, reverse));
            rgb += fill_cols;
            col += fill_cols;
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
            if (part > fill_cols)
                part = fill_cols;
            fill_cols -= part;
            col += part;
            if (bpp == 1 && reverse)
                mode_emit_head_1bpp_reverse(&rgb, *data++, pal, start, part);
            else if (bpp == 1)
                mode_emit_head_1bpp(&rgb, *data++, pal, start, part);
            else if (reverse)
                mode_emit_head_2bpp_reverse(&rgb, *data++, pal, start, part);
            else
                mode_emit_head_2bpp(&rgb, *data++, pal, start, part);
            col += fill_cols;
            const unsigned bytes = (unsigned)fill_cols / ppb;
            if (bpp == 1)
                mode_run_1bpp(rgb, data, bytes, tbl, reverse);
            else
                mode_run_2bpp(rgb, data, bytes, tbl, reverse);
            rgb += bytes * ppb;
            data += bytes;
            fill_cols -= bytes * ppb;
            if (bpp == 1 && reverse)
                mode_emit_tail_1bpp_reverse(&rgb, *data, pal, fill_cols);
            else if (bpp == 1)
                mode_emit_tail_1bpp(&rgb, *data, pal, fill_cols);
            else if (reverse)
                mode_emit_tail_2bpp_reverse(&rgb, *data, pal, fill_cols);
            else
                mode_emit_tail_2bpp(&rgb, *data, pal, fill_cols);
        }
    }
    return true;
}

#define MODE3_RENDER_FN(name, bpp, reverse)                                    \
    static bool name(int16_t plane_id, int16_t scanline_id, int16_t width,     \
                     uint16_t *rgb, uint16_t config_ptr,                       \
                     vga_fill_cache_t *cache)                                  \
    {                                                                          \
        (void)plane_id;                                                        \
        return mode3_render(scanline_id, width, rgb, config_ptr, cache,        \
                            bpp, reverse);                                     \
    }
MODE3_RENDER_FN(mode3_render_1bpp, 1, false)
MODE3_RENDER_FN(mode3_render_2bpp, 2, false)
MODE3_RENDER_FN(mode3_render_4bpp, 4, false)
MODE3_RENDER_FN(mode3_render_8bpp, 8, false)
MODE3_RENDER_FN(mode3_render_16bpp, 16, false)
MODE3_RENDER_FN(mode3_render_1bpp_reverse, 1, true)
MODE3_RENDER_FN(mode3_render_2bpp_reverse, 2, true)
MODE3_RENDER_FN(mode3_render_4bpp_reverse, 4, true)
#undef MODE3_RENDER_FN

/* Every attribute this mode defines and the renderer it names is written here
 * once. mode3_fill_fn and mode3_fill_valid both expand this list, so
 * neither can drift from the other.
 *
 * A fabric build expands only the attribute column. Nothing there calls these
 * renderers, and that image has one 96 KB memory for text, stack and heap. */
#define MODE3_FILLS(F) \
    F(0, mode3_render_1bpp)          \
    F(1, mode3_render_2bpp)          \
    F(2, mode3_render_4bpp)          \
    F(3, mode3_render_8bpp)          \
    F(4, mode3_render_16bpp)         \
    F(8, mode3_render_1bpp_reverse)  \
    F(9, mode3_render_2bpp_reverse)  \
    F(10, mode3_render_4bpp_reverse)

bool mode3_fill_valid(uint16_t attributes)
{
    switch (attributes)
    {
#define MODE3_CASE(attr, fn) case attr:
        MODE3_FILLS(MODE3_CASE)
#undef MODE3_CASE
        return true;
    default:
        return false;
    }
}

#ifdef RP6502_VGA_FABRIC

vga_fill_fn_t mode3_fill_fn(uint16_t attributes)
{
    (void)attributes;
    return NULL;
}

#else

vga_fill_fn_t mode3_fill_fn(uint16_t attributes)
{
    switch (attributes)
    {
#define MODE3_CASE(attr, fn) \
    case attr:                \
        return fn;
        MODE3_FILLS(MODE3_CASE)
#undef MODE3_CASE
    default:
        return NULL;
    }
}

#endif

#ifndef RP6502_VGA_FABRIC
bool mode3_fill_attr(vga_fill_fn_t fn, uint16_t *attributes)
{
    for (uint16_t a = 0; a < 16; a++)
        if (fn && mode3_fill_fn(a) == fn)
        {
            *attributes = a;
            return true;
        }
    return false;
}
#endif

bool mode3_prog(uint16_t *xregs)
{
    const uint16_t attributes = xregs[2];
    const uint16_t config_ptr = xregs[3];
    const int16_t plane = xregs[4];
    const int16_t scanline_begin = xregs[5];
    const int16_t scanline_end = xregs[6];

    if (config_ptr & 1 ||
        config_ptr > 0x10000 - sizeof(mode3_config_t))
        return false;

    if (!mode3_fill_valid(attributes))
        return false;
    vga_fill_fn_t render_fn = mode3_fill_fn(attributes);

    return vga_prog_fill(plane, scanline_begin, scanline_end, config_ptr, render_fn);
}

#pragma GCC pop_options
