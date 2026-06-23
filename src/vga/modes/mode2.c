/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/mode2.h"
#include "modes/modes.h"
#include "sys/vga.h"
#include "sys/mem.h"
#include "scanvideo/scanvideo.h"
#include "term/color.h"
#include <string.h>

#pragma GCC push_options
#pragma GCC optimize("O3")

typedef struct
{
    bool x_wrap;
    bool y_wrap;
    int16_t x_pos_px;
    int16_t y_pos_px;
    int16_t width_tiles;
    int16_t height_tiles;
    uint16_t xram_data_ptr;
    uint16_t xram_palette_ptr;
    uint16_t xram_tile_ptr;
} mode2_config_t;

// Per-scanline, per-plane validated mode-2 OPTIONS word (bpp, tile size, x/y trim).
// It is the single source of truth for the selector: mode2_render loads it with one
// atomic access and derives tile_size and trim from the same word, so a read racing a
// concurrent reprogram can never pair a tile_size with an out-of-range trim.
static uint16_t mode2_options[VGA_PROG_MAX][SCANVIDEO_PLANE_COUNT];

// tile_h is the on-screen tile height: the base tile_size when untrimmed, less
// when Y-trimmed (then non-power-of-2, hence the modulo). *row is left holding the
// row within the tile.
static volatile const uint8_t *
mode2_scanline_to_data(int16_t scanline_id, mode2_config_t *config, int16_t tile_h, int16_t *row)
{
    *row = scanline_id - config->y_pos_px;
    const int16_t height = config->height_tiles * tile_h;
    if (config->y_wrap)
    {
        if (*row < 0)
            *row += (-(*row + 1) / height + 1) * height;
        if (*row >= height)
            *row -= ((*row - height) / height + 1) * height;
    }
    if (*row < 0 || *row >= height || config->width_tiles < 1 || height < 1)
        return NULL;
    const uint32_t sizeof_row = (uint32_t)config->width_tiles;
    const uint32_t sizeof_bitmap = (uint32_t)config->height_tiles * sizeof_row;
    if (sizeof_bitmap > (uint32_t)(0x10000 - config->xram_data_ptr))
        return NULL;
    volatile const uint8_t *rv = &xram[config->xram_data_ptr + *row / tile_h * sizeof_row];
    *row %= tile_h;
    return rv;
}

static volatile const uint16_t *
mode2_get_palette(mode2_config_t *config, int16_t bpp)
{
    if (!(config->xram_palette_ptr & 1) &&
        config->xram_palette_ptr <= 0x10000 - sizeof(uint16_t) * (1 << bpp))
        return (uint16_t *)&xram[config->xram_palette_ptr];
    if (bpp == 1)
        return color_2;
    return color_256;
}

static inline __attribute__((always_inline)) int16_t
mode2_fill_cols(mode2_config_t *config, uint16_t **rgb, int16_t *col, int16_t *width, int16_t tile_size)
{
    int16_t width_px = config->width_tiles * tile_size;
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

static inline __attribute__((always_inline)) uint32_t
mode2_get_tile_row_addr(mode2_config_t *config, int16_t bpp, int16_t tile_size,
                        int16_t col, int16_t row, volatile const uint8_t *row_data, uint16_t *index)
{
    uint32_t row_size = tile_size * bpp / 8;
    uint32_t mem_size = row_size * tile_size;
    uint8_t tile_id = row_data[col / tile_size];
    uint8_t pixels_per_byte = 8 / bpp;
    *index = (col / pixels_per_byte) & (tile_size / pixels_per_byte - 1);
    return (uint32_t)config->xram_tile_ptr + mem_size * tile_id + row_size * row;
}

// Untrimmed emission: on-screen tiles align to data bytes, so each byte yields a
// whole run of pixels (8 at 1bpp, 4 at 2bpp, ...) read from volatile xram once
// rather than once per pixel. bpp/tile_size are compile-time, so each instantiation
// folds to a single bpp's byte-walk.
static inline __attribute__((always_inline)) void
mode2_emit_full(mode2_config_t *config, uint16_t *rgb, int16_t width,
                volatile const uint8_t *row_data, int16_t row,
                const uint16_t *pal, int16_t tile_size, int16_t bpp)
{
    int16_t col = -config->x_pos_px;
    if (bpp == 1)
    {
        while (width)
        {
            int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width, tile_size);
            uint16_t index;
            uint32_t tile_mem = mode2_get_tile_row_addr(config, 1, tile_size, col, row, row_data, &index);
            uint8_t bits = xram[tile_mem + index];
            int16_t start = col & 7;
            int16_t part = 8 - start;
            if (part > fill_cols)
                part = fill_cols;
            fill_cols -= part;
            col += part;
            modes_emit_head_1bpp(&rgb, bits, pal, start, part);
            if (++index == tile_size / 8)
                tile_mem = mode2_get_tile_row_addr(config, 1, tile_size, col, row, row_data, &index);
            bits = xram[tile_mem + index];
            while (fill_cols > 7)
            {
                modes_render_1bpp(rgb, bits, pal[0], pal[1]);
                rgb += 8;
                fill_cols -= 8;
                col += 8;
                if (++index == tile_size / 8)
                    tile_mem = mode2_get_tile_row_addr(config, 1, tile_size, col, row, row_data, &index);
                bits = xram[tile_mem + index];
            }
            col += fill_cols;
            modes_emit_tail_1bpp(&rgb, bits, pal, fill_cols);
        }
    }
    else if (bpp == 2)
    {
        while (width)
        {
            int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width, tile_size);
            uint16_t index;
            uint32_t tile_mem = mode2_get_tile_row_addr(config, 2, tile_size, col, row, row_data, &index);
            uint8_t bits = xram[tile_mem + index];
            int16_t start = col & 3;
            int16_t part = 4 - start;
            if (part > fill_cols)
                part = fill_cols;
            fill_cols -= part;
            col += part;
            modes_emit_head_2bpp(&rgb, bits, pal, start, part);
            if (++index == tile_size / 4)
                tile_mem = mode2_get_tile_row_addr(config, 2, tile_size, col, row, row_data, &index);
            bits = xram[tile_mem + index];
            while (fill_cols > 3)
            {
                *rgb++ = pal[(bits & 0xC0) >> 6];
                *rgb++ = pal[(bits & 0x30) >> 4];
                *rgb++ = pal[(bits & 0x0C) >> 2];
                *rgb++ = pal[bits & 0x03];
                fill_cols -= 4;
                col += 4;
                if (++index == tile_size / 4)
                    tile_mem = mode2_get_tile_row_addr(config, 2, tile_size, col, row, row_data, &index);
                bits = xram[tile_mem + index];
            }
            col += fill_cols;
            modes_emit_tail_2bpp(&rgb, bits, pal, fill_cols);
        }
    }
    else if (bpp == 4)
    {
        while (width)
        {
            int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width, tile_size);
            uint16_t index;
            uint32_t tile_mem = mode2_get_tile_row_addr(config, 4, tile_size, col, row, row_data, &index);
            uint8_t bits = xram[tile_mem + index];
            if (col & 1)
            {
                *rgb++ = pal[bits & 0xF];
                col++;
                fill_cols--;
                if (++index == tile_size / 2)
                    tile_mem = mode2_get_tile_row_addr(config, 4, tile_size, col, row, row_data, &index);
                bits = xram[tile_mem + index];
            }
            while (fill_cols > 1)
            {
                *rgb++ = pal[bits >> 4];
                *rgb++ = pal[bits & 0xF];
                fill_cols -= 2;
                col += 2;
                if (++index == tile_size / 2)
                    tile_mem = mode2_get_tile_row_addr(config, 4, tile_size, col, row, row_data, &index);
                bits = xram[tile_mem + index];
            }
            col += fill_cols;
            if (fill_cols == 1)
                *rgb++ = pal[bits >> 4];
        }
    }
    else
    {
        while (width)
        {
            int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width, tile_size);
            uint16_t index;
            uint32_t tile_mem = mode2_get_tile_row_addr(config, 8, tile_size, col, row, row_data, &index);
            uint8_t bits = xram[tile_mem + index];
            while (fill_cols > 0)
            {
                *rgb++ = pal[bits];
                fill_cols -= 1;
                col += 1;
                if (++index == tile_size)
                    tile_mem = mode2_get_tile_row_addr(config, 8, tile_size, col, row, row_data, &index);
                bits = xram[tile_mem + index];
            }
        }
    }
}

// Trimmed emission. The effective tile is eff_w wide (< tile_size), so on-screen
// tiles no longer align to data bytes and the byte-walk above can't be used;
// pixels are read individually. Tile data is still stored at the full tile_size.
static inline __attribute__((always_inline)) void
mode2_emit_trim(mode2_config_t *config, uint16_t *rgb, int16_t width,
                volatile const uint8_t *row_data, int16_t row, int16_t eff_w,
                const uint16_t *pal, int16_t tile_size, int16_t bpp)
{
    const uint32_t row_size = (uint32_t)tile_size * bpp / 8;
    const uint32_t mem_size = row_size * tile_size;
    const uint32_t row_off = (uint32_t)config->xram_tile_ptr + row_size * row;
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width, eff_w);
        while (fill_cols > 0)
        {
            const int16_t col_in_tile = col % eff_w;
            const uint32_t tile_mem = row_off + mem_size * row_data[col / eff_w];
            int16_t run = eff_w - col_in_tile;
            if (run > fill_cols)
                run = fill_cols;
            col += run;
            fill_cols -= run;
            for (int16_t px = col_in_tile; px < col_in_tile + run; px++)
            {
                uint8_t idx;
                if (bpp == 1)
                    idx = (xram[tile_mem + (px >> 3)] >> (7 - (px & 7))) & 0x01;
                else if (bpp == 2)
                    idx = (xram[tile_mem + (px >> 2)] >> (6 - 2 * (px & 3))) & 0x03;
                else if (bpp == 4)
                    idx = (px & 1) ? (xram[tile_mem + (px >> 1)] & 0x0F)
                                   : (xram[tile_mem + (px >> 1)] >> 4);
                else
                    idx = xram[tile_mem + px];
                *rgb++ = pal[idx];
            }
        }
    }
}

static inline __attribute__((always_inline)) bool
mode2_render_1bpp(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr,
                  int16_t x_trim, int16_t y_trim, int16_t tile_size)
{
    mode2_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, tile_size - y_trim, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode2_get_palette(config, 1);
    uint16_t pal[2] = {palette[0], palette[1]};
    if (x_trim || y_trim)
        mode2_emit_trim(config, rgb, width, row_data, row, tile_size - x_trim, pal, tile_size, 1);
    else
        mode2_emit_full(config, rgb, width, row_data, row, pal, tile_size, 1);
    return true;
}

static inline __attribute__((always_inline)) bool
mode2_render_2bpp(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr,
                  int16_t x_trim, int16_t y_trim, int16_t tile_size)
{
    mode2_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, tile_size - y_trim, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode2_get_palette(config, 2);
    uint16_t pal[4] = {palette[0], palette[1], palette[2], palette[3]};
    if (x_trim || y_trim)
        mode2_emit_trim(config, rgb, width, row_data, row, tile_size - x_trim, pal, tile_size, 2);
    else
        mode2_emit_full(config, rgb, width, row_data, row, pal, tile_size, 2);
    return true;
}

static inline __attribute__((always_inline)) bool
mode2_render_4bpp(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr,
                  int16_t x_trim, int16_t y_trim, int16_t tile_size)
{
    mode2_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, tile_size - y_trim, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode2_get_palette(config, 4);
    uint16_t pal[16];
    for (int i = 0; i < 16; i++)
        pal[i] = palette[i];
    if (x_trim || y_trim)
        mode2_emit_trim(config, rgb, width, row_data, row, tile_size - x_trim, pal, tile_size, 4);
    else
        mode2_emit_full(config, rgb, width, row_data, row, pal, tile_size, 4);
    return true;
}

static inline __attribute__((always_inline)) bool
mode2_render_8bpp(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr,
                  int16_t x_trim, int16_t y_trim, int16_t tile_size)
{
    mode2_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, tile_size - y_trim, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode2_get_palette(config, 8);
    uint16_t pal[256];
    for (int i = 0; i < 256; i++)
        pal[i] = palette[i];
    if (x_trim || y_trim)
        mode2_emit_trim(config, rgb, width, row_data, row, tile_size - x_trim, pal, tile_size, 8);
    else
        mode2_emit_full(config, rgb, width, row_data, row, pal, tile_size, 8);
    return true;
}

// Single fill_fn for every mode-2 scanline. The whole selector (bpp, tile size, trim)
// comes from one atomically-loaded options word, so tile_size and trim are always the
// same validated generation; the switch keeps bpp/tile_size compile-time per branch.
static bool
mode2_render(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    const uint16_t opt = mode2_options[scanline_id][plane_id];
    const int16_t x_trim = (opt >> 4) & 0x0F;
    const int16_t y_trim = (opt >> 8) & 0x0F;
    switch (opt & 0x0F)
    {
    case 0:
        return mode2_render_1bpp(scanline_id, width, rgb, config_ptr, x_trim, y_trim, 8);
    case 1:
        return mode2_render_2bpp(scanline_id, width, rgb, config_ptr, x_trim, y_trim, 8);
    case 2:
        return mode2_render_4bpp(scanline_id, width, rgb, config_ptr, x_trim, y_trim, 8);
    case 3:
        return mode2_render_8bpp(scanline_id, width, rgb, config_ptr, x_trim, y_trim, 8);
    case 8:
        return mode2_render_1bpp(scanline_id, width, rgb, config_ptr, x_trim, y_trim, 16);
    case 9:
        return mode2_render_2bpp(scanline_id, width, rgb, config_ptr, x_trim, y_trim, 16);
    case 10:
        return mode2_render_4bpp(scanline_id, width, rgb, config_ptr, x_trim, y_trim, 16);
    case 11:
        return mode2_render_8bpp(scanline_id, width, rgb, config_ptr, x_trim, y_trim, 16);
    default:
        return false;
    }
}

bool mode2_prog(uint16_t *xregs)
{
    const uint16_t options = xregs[2];
    const uint16_t config_ptr = xregs[3];
    const int16_t plane = xregs[4];
    const int16_t scanline_begin = xregs[5];
    const int16_t scanline_end = xregs[6];

    if (config_ptr & 1 ||
        config_ptr > 0x10000 - sizeof(mode2_config_t))
        return false;
    if (options & 0xF000 || (options & 0x07) > 3 ||
        plane < 0 || plane >= SCANVIDEO_PLANE_COUNT)
        return false;
    const int16_t tile_size = (options & 0x08) ? 16 : 8;
    const int16_t x_trim = (options >> 4) & 0x0F;
    const int16_t y_trim = (options >> 8) & 0x0F;
    if (x_trim >= tile_size || y_trim >= tile_size)
        return false;

    const int16_t end = scanline_end ? scanline_end : vga_canvas_height();
    if (!vga_prog_fill(plane, scanline_begin, end, config_ptr, mode2_render))
        return false;
    for (int16_t i = scanline_begin; i < end; i++)
        mode2_options[i][plane] = options;
    return true;
}

#pragma GCC pop_options
