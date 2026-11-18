/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/mode/mode1.h"
#include "core/vga/mode/mode.h"
#include "core/vga/vga.h"
#include "core/sys/xram.h"
#include "core/term/color.h"
#include "core/term/font.h"
#include <string.h>

#pragma GCC push_options
#pragma GCC optimize("O3")

typedef struct
{
    bool x_wrap;
    bool y_wrap;
    int16_t x_pos_px;
    int16_t y_pos_px;
    int16_t width_chars;
    int16_t height_chars;
    uint16_t xram_data_ptr;
    uint16_t xram_palette_ptr;
    uint16_t xram_font_ptr;
} mode1_config_t;

/* Each renderer copies the config out of XRAM once per scanline, checks the
 * copy and draws from it. A write to XRAM part way through a line, such as one
 * byte of a 16-bit size, then cannot slip in behind the checks. */

typedef struct
{
    uint8_t glyph_code;
} mode1_1bpp_data_t;

typedef struct
{
    uint8_t glyph_code;
    uint8_t fg_bg_index;
} mode1_4bppr_data_t;

typedef struct
{
    uint8_t glyph_code;
    uint8_t bg_fg_index;
} mode1_4bpp_data_t;

typedef struct
{
    uint8_t glyph_code;
    uint8_t fg_index;
    uint8_t bg_index;
} mode1_8bpp_data_t;

typedef struct
{
    uint8_t glyph_code;
    uint8_t attributes;
    uint16_t fg_color;
    uint16_t bg_color;
} mode1_16bpp_data_t;

static const uint8_t *
mode1_scanline_to_data(int16_t scanline_id, int16_t width, mode1_config_t *config, size_t cell_size, int16_t font_height, int16_t *row)
{
    // The size in pixels has to fit an int16_t, and be at least 1, before the
    // wrap divides by it. The counts alone would let 4096 cells overflow.
    if (config->width_chars < 1 || config->width_chars > INT16_MAX / 8 ||
        config->height_chars < 1 ||
        (int32_t)config->height_chars * font_height > INT16_MAX)
        return NULL;
    *row = scanline_id - config->y_pos_px;
    const int16_t height = config->height_chars * font_height;
    if (config->y_wrap)
    {
        if (*row < 0)
            *row += (-(*row + 1) / height + 1) * height;
        if (*row >= height)
            *row -= ((*row - height) / height + 1) * height;
    }
    if (*row < 0 || *row >= height)
        return NULL;
    // A layer that misses the scanline across, like one that misses it down,
    // draws nothing. mode1_fill_cols counts on it: unwrapped, a column past
    // the right of the cells would leave it a negative run.
    if (!config->x_wrap &&
        (config->x_pos_px >= width || config->x_pos_px + config->width_chars * 8 <= 0))
        return NULL;
    const uint32_t sizeof_row = (uint32_t)config->width_chars * cell_size;
    const uint32_t sizeof_bitmap = (uint32_t)config->height_chars * sizeof_row;
    if (sizeof_bitmap > (uint32_t)(0x10000 - config->xram_data_ptr))
        return NULL;
    const uint8_t *rv = (const uint8_t *)&xram[config->xram_data_ptr + *row / font_height * sizeof_row];
    *row &= font_height - 1;
    return rv;
}

static const uint16_t *
mode1_get_palette(mode1_config_t *config, int16_t bpp)
{
    if (!(config->xram_palette_ptr & 1) &&
        config->xram_palette_ptr <= 0x10000 - sizeof(uint16_t) * (1 << bpp))
        return (const uint16_t *)&xram[config->xram_palette_ptr];
    if (bpp == 1)
        return color_2;
    return color_256;
}

static const uint8_t *
mode1_get_font(mode1_config_t *config, int16_t font_height)
{
    if (config->xram_font_ptr <= 0x10000 - 256 * font_height)
        return (const uint8_t *)&xram[config->xram_font_ptr];
    if (font_height == 8)
        return font8;
    return font16;
}

static inline __attribute__((always_inline)) int16_t
mode1_fill_cols(mode1_config_t *config, uint16_t **rgb, int16_t *col, int16_t *width)
{
    int16_t width_px = config->width_chars * 8;
    if (*col < 0)
    {
        if (config->x_wrap)
            *col += (-(*col + 1) / width_px + 1) * width_px;
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
    if (*col >= width_px)
    {
        if (config->x_wrap)
            *col -= ((*col - width_px) / width_px + 1) * width_px;
        else
        {
            memset(*rgb, 0, sizeof(uint16_t) * (*width));
            *width = 0;
        }
    }
    int16_t fill_cols = *width;
    if (fill_cols > width_px - *col)
        fill_cols = width_px - *col;
    *width -= fill_cols;
    return fill_cols;
}

static inline __attribute__((always_inline)) bool
mode1_render_1bpp(int16_t scanline_id, int16_t width, uint16_t *rgb,
                  uint16_t config_ptr, int16_t font_height)
{
    mode1_config_t cfg = *(const mode1_config_t *)&xram[config_ptr];
    mode1_config_t *config = &cfg;
    int16_t row;
    const mode1_1bpp_data_t *row_data =
        (void *)mode1_scanline_to_data(scanline_id, width, config, sizeof(mode1_1bpp_data_t), font_height, &row);
    if (!row_data)
        return false;
    const uint16_t *palette = mode1_get_palette(config, 1);
    const uint8_t *font = mode1_get_font(config, font_height) + 256 * row;
    uint16_t colors[2] = {palette[0], palette[1]};
    MODE_TABLE(pair, 4);
    mode_pair_set(pair, colors[0], colors[1]);
    int16_t col = -config->x_pos_px;
    int16_t width_px = config->width_chars * 8;
    while (width)
    {
        int16_t fill_cols = mode1_fill_cols(config, &rgb, &col, &width);
        const mode1_1bpp_data_t *data = &row_data[col / 8];
        uint8_t glyph = font[data->glyph_code];
        int16_t start = col & 7;
        int16_t part = 8 - start;
        if (part > width_px - col)
            part = width_px - col;
        if (part > fill_cols)
            part = fill_cols;
        fill_cols -= part;
        col += part;
        mode_emit_head_1bpp(&rgb, glyph, colors, start, part);
        glyph = font[(++data)->glyph_code];
        col += fill_cols;
        while (fill_cols > 7)
        {
            mode_render_1bpp(rgb, glyph, pair);
            rgb += 8;
            fill_cols -= 8;
            glyph = font[(++data)->glyph_code];
        }
        mode_emit_tail_1bpp(&rgb, glyph, colors, fill_cols);
    }
    return true;
}

static bool
mode1_render_1bpp_8x8(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr,
                     vga_fill_cache_t *cache)
{
    (void)plane_id;
    (void)cache;
    return mode1_render_1bpp(scanline_id, width, rgb, config_ptr, 8);
}

static bool
mode1_render_1bpp_8x16(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr,
                     vga_fill_cache_t *cache)
{
    (void)plane_id;
    (void)cache;
    return mode1_render_1bpp(scanline_id, width, rgb, config_ptr, 16);
}

/* The depths with a color in every cell, in the order of their cells' sizes.
 * Each is also the first word of the key of its rows in a plane's fill
 * cache, where mode 0's is 1; see vga_fill_cache_t. */
enum
{
    MODE1_4BPPR = 2,
    MODE1_4BPP,
    MODE1_8BPP,
    MODE1_16BPP,
};

/* The four ways two pixels of a cell can land, in the cell's colors. */
static inline __attribute__((always_inline)) void
mode1_cell_pair(uint32_t *pair, const uint8_t *cell, const uint16_t *palette, const int depth)
{
    uint16_t bg, fg;
    if (depth == MODE1_4BPPR)
    {
        const mode1_4bppr_data_t *data = (const void *)cell;
        bg = palette[data->fg_bg_index & 0xF];
        fg = palette[data->fg_bg_index >> 4];
    }
    else if (depth == MODE1_4BPP)
    {
        const mode1_4bpp_data_t *data = (const void *)cell;
        bg = palette[data->bg_fg_index >> 4];
        fg = palette[data->bg_fg_index & 0xF];
    }
    else if (depth == MODE1_8BPP)
    {
        const mode1_8bpp_data_t *data = (const void *)cell;
        bg = palette[data->bg_index];
        fg = palette[data->fg_index];
    }
    else
    {
        const mode1_16bpp_data_t *data = (const void *)cell;
        bg = data->bg_color;
        fg = data->fg_color;
    }
    mode_pair_set(pair, bg, fg);
}

/* A scanline of colored cells. A row of cells is font_height scanlines in
 * the same colors, so the pairs of every cell across the line are built in
 * the plane's fill cache for the first scanline a thread draws of the row,
 * and the rest only look glyphs up. They are kept by position across the
 * line, left to right, and built again when the frame, the row, the depth
 * or anything in the config differs from what they were built for. */
static inline __attribute__((always_inline)) bool
mode1_render_color(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr,
                   vga_fill_cache_t *cache, const int16_t font_height, const int depth)
{
    const size_t cell_size = depth == MODE1_16BPP  ? sizeof(mode1_16bpp_data_t)
                             : depth == MODE1_8BPP ? sizeof(mode1_8bpp_data_t)
                                                   : sizeof(mode1_4bpp_data_t);
    mode1_config_t cfg = *(const mode1_config_t *)&xram[config_ptr];
    mode1_config_t *config = &cfg;
    int16_t row;
    const uint8_t *row_data =
        mode1_scanline_to_data(scanline_id, width, config, cell_size, font_height, &row);
    if (!row_data)
        return false;
    const uint16_t *palette =
        depth == MODE1_16BPP ? NULL : mode1_get_palette(config, depth == MODE1_8BPP ? 8 : 4);
    const uint8_t *font = mode1_get_font(config, font_height) + 256 * row;
    uint32_t key[7] = {(uint32_t)depth, (uint32_t)vga_frame_count(),
                       (uint32_t)(row_data - (const uint8_t *)xram)};
    _Static_assert(sizeof(cfg) <= 4 * sizeof(uint32_t), "the config fits the key");
    memcpy(&key[3], &cfg, sizeof(cfg));
    const bool build = memcmp(cache->row_key, key, sizeof(key)) != 0;
    if (build)
        memcpy(cache->row_key, key, sizeof(key));
    /* Only the first and the last cell across a line can be cut, because
     * the cells wrap on a multiple of 8 pixels, so a line 640 wide has at
     * most 81. */
    _Static_assert(VGA_FILL_ROW_CELLS >= 640 / 8 + 1, "a pair table for every cell across a line");
    uint32_t *pair = cache->row_pair[0];
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode1_fill_cols(config, &rgb, &col, &width);
        if (fill_cols <= 0)
            continue;
        const uint8_t *data = row_data + (col / 8) * cell_size;
        const int16_t start = col & 7;
        col += fill_cols;
        if (start)
        {
            /* The cell cut by the left of the line. */
            int16_t part = 8 - start;
            if (part > fill_cols)
                part = fill_cols;
            if (build)
                mode1_cell_pair(pair, data, palette, depth);
            const uint16_t colors[2] = {(uint16_t)pair[0], (uint16_t)pair[3]};
            mode_emit_head_1bpp(&rgb, font[*data], colors, start, part);
            fill_cols -= part;
            data += cell_size;
            pair += 4;
        }
        if (build)
        {
            /* The whole cells, and the one cut by the right of the line. */
            const uint8_t *cell = data;
            uint32_t *p = pair;
            for (int16_t n = (fill_cols + 7) / 8; n; n--, cell += cell_size, p += 4)
                mode1_cell_pair(p, cell, palette, depth);
        }
        for (; fill_cols > 7; fill_cols -= 8, rgb += 8, data += cell_size, pair += 4)
            mode_render_1bpp(rgb, font[*data], pair);
        if (fill_cols)
        {
            const uint16_t colors[2] = {(uint16_t)pair[0], (uint16_t)pair[3]};
            mode_emit_tail_1bpp(&rgb, font[*data], colors, fill_cols);
            pair += 4;
        }
    }
    return true;
}

#define MODE1_COLOR_FN(name, font_height, depth)                               \
    static bool                                                                \
    name(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb,  \
         uint16_t config_ptr, vga_fill_cache_t *cache)                         \
    {                                                                          \
        (void)plane_id;                                                        \
        return mode1_render_color(scanline_id, width, rgb, config_ptr, cache,  \
                                  font_height, depth);                         \
    }
MODE1_COLOR_FN(mode1_render_4bppr_8x8, 8, MODE1_4BPPR)
MODE1_COLOR_FN(mode1_render_4bppr_8x16, 16, MODE1_4BPPR)
MODE1_COLOR_FN(mode1_render_4bpp_8x8, 8, MODE1_4BPP)
MODE1_COLOR_FN(mode1_render_4bpp_8x16, 16, MODE1_4BPP)
MODE1_COLOR_FN(mode1_render_8bpp_8x8, 8, MODE1_8BPP)
MODE1_COLOR_FN(mode1_render_8bpp_8x16, 16, MODE1_8BPP)
MODE1_COLOR_FN(mode1_render_16bpp_8x8, 8, MODE1_16BPP)
MODE1_COLOR_FN(mode1_render_16bpp_8x16, 16, MODE1_16BPP)
#undef MODE1_COLOR_FN

/* Every attribute this mode defines and the renderer it names is written here
 * once. mode1_fill_fn and mode1_fill_valid both expand this list, so
 * neither can drift from the other.
 *
 * A fabric build expands only the attribute column. Naming a renderer would
 * keep mode1_get_font, and with it font8 and font16, which core/term/font.c
 * defines and a fabric image does not compile. */
#define MODE1_FILLS(F)              \
    F(0, mode1_render_1bpp_8x8)     \
    F(1, mode1_render_4bppr_8x8)    \
    F(2, mode1_render_4bpp_8x8)     \
    F(3, mode1_render_8bpp_8x8)     \
    F(4, mode1_render_16bpp_8x8)    \
    F(8, mode1_render_1bpp_8x16)    \
    F(9, mode1_render_4bppr_8x16)   \
    F(10, mode1_render_4bpp_8x16)   \
    F(11, mode1_render_8bpp_8x16)   \
    F(12, mode1_render_16bpp_8x16)

bool mode1_fill_valid(uint16_t attributes)
{
    switch (attributes)
    {
#define MODE1_CASE(attr, fn) case attr:
        MODE1_FILLS(MODE1_CASE)
#undef MODE1_CASE
        return true;
    default:
        return false;
    }
}

#ifdef RP6502_VGA_FABRIC

vga_fill_fn_t mode1_fill_fn(uint16_t attributes)
{
    (void)attributes;
    return NULL;
}

#else

vga_fill_fn_t mode1_fill_fn(uint16_t attributes)
{
    switch (attributes)
    {
#define MODE1_CASE(attr, fn) \
    case attr:               \
        return fn;
        MODE1_FILLS(MODE1_CASE)
#undef MODE1_CASE
    default:
        return NULL;
    }
}

#endif

#ifndef RP6502_VGA_FABRIC
bool mode1_fill_attr(vga_fill_fn_t fn, uint16_t *attributes)
{
    for (uint16_t a = 0; a < 16; a++)
        if (fn && mode1_fill_fn(a) == fn)
        {
            *attributes = a;
            return true;
        }
    return false;
}
#endif

bool mode1_prog(uint16_t *xregs)
{
    const uint16_t attributes = xregs[2];
    const uint16_t config_ptr = xregs[3];
    const int16_t plane = xregs[4];
    const int16_t scanline_begin = xregs[5];
    const int16_t scanline_end = xregs[6];

    if (config_ptr & 1 ||
        config_ptr > 0x10000 - sizeof(mode1_config_t))
        return false;

    if (!mode1_fill_valid(attributes))
        return false;

    return vga_prog_fill(plane, scanline_begin, scanline_end, config_ptr,
                         mode1_fill_fn(attributes));
}

#pragma GCC pop_options
