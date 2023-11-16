/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/mode2.h"
#include "modes/modes.h"
#include "sys/vga.h"
#include "sys/mem.h"
#include "term/color.h"
#include "pico/scanvideo.h"
#include <string.h>

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

static volatile const uint8_t *__attribute__((optimize("O1")))
mode2_scanline_to_data(int16_t scanline_id, mode2_config_t *config, size_t cell_size, int16_t font_height, int16_t *row)
{
    *row = scanline_id - config->y_pos_px;
    const int16_t height = config->height_tiles * font_height;
    if (config->y_wrap)
    {
        if (*row < 0)
            *row += (-(*row + 1) / height + 1) * height;
        if (*row >= height)
            *row -= ((*row - height) / height + 1) * height;
    }
    if (*row < 0 || *row >= height || config->width_tiles < 1 || height < 1)
        return NULL;
    const int32_t sizeof_row = (int32_t)config->width_tiles * cell_size;
    const int32_t sizeof_bitmap = (int32_t)config->height_tiles * sizeof_row;
    if (sizeof_bitmap > 0x10000 - config->xram_data_ptr)
        return NULL;
    volatile const uint8_t *rv = &xram[config->xram_data_ptr + *row / font_height * sizeof_row];
    *row &= font_height - 1;
    return rv;
}

static volatile const uint16_t *__attribute__((optimize("O1")))
mode2_get_palette(mode2_config_t *config, int16_t bpp)
{
    if (!(config->xram_palette_ptr & 1) &&
        config->xram_palette_ptr <= 0x10000 - sizeof(uint16_t) * (2 ^ bpp))
        return (uint16_t *)&xram[config->xram_palette_ptr];
    if (bpp == 1)
        return color_2;
    return color_256;
}

static inline __attribute__((always_inline)) int16_t __attribute__((optimize("O1")))
mode2_fill_cols(mode2_config_t *config, uint16_t **rgb, int16_t *col, int16_t *width)
{
    int16_t width_px = config->width_tiles * 8;
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

static inline __attribute__((always_inline)) uint16_t __attribute__((optimize("O1")))
mode2_get_glyph_tile_mem(mode2_config_t *config, int16_t bpp, int16_t tile_size,
                         int16_t col, int16_t row, volatile const uint8_t *row_data, uint16_t *index)
{
    uint32_t row_size = (tile_size == 8) ? bpp : 2 * bpp;
    uint32_t mem_size = row_size * tile_size;
    uint8_t tile_id = row_data[col / tile_size];
    uint8_t pixels_per_byte = 8 / bpp;
    *index = (col / pixels_per_byte) & (tile_size / pixels_per_byte - 1);
    return (uint32_t)config->xram_tile_ptr + mem_size * tile_id + row_size * row;
}

static inline __attribute__((always_inline)) bool __attribute__((optimize("O1")))
mode2_render_1bpp(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr, int16_t tile_size)
{
    if (config_ptr > 0x10000 - sizeof(mode2_config_t))
        return false;
    mode2_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, sizeof(uint8_t), tile_size, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode2_get_palette(config, 1);
    int16_t col = -config->x_pos_px;

    while (width)
    {
        int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width);
        uint16_t index;
        uint16_t tile_mem = mode2_get_glyph_tile_mem(config, 1, tile_size, col, row, row_data, &index);
        uint8_t glyph = xram[tile_mem + index];
        int16_t part = 8 - (col & 7);
        fill_cols -= part;
        col += part;
        switch (part)
        {
        case 8:
            *rgb++ = palette[(glyph & 0x80) >> 7];
        case 7:
            *rgb++ = palette[(glyph & 0x40) >> 6];
        case 6:
            *rgb++ = palette[(glyph & 0x20) >> 5];
        case 5:
            *rgb++ = palette[(glyph & 0x10) >> 4];
        case 4:
            *rgb++ = palette[(glyph & 0x08) >> 3];
        case 3:
            *rgb++ = palette[(glyph & 0x04) >> 2];
        case 2:
            *rgb++ = palette[(glyph & 0x02) >> 1];
        case 1:
            *rgb++ = palette[glyph & 0x01];
            tile_mem = mode2_get_glyph_tile_mem(config, 1, tile_size, col, row, row_data, &index);
            glyph = xram[tile_mem + index];
        }
        while (fill_cols > 7)
        {
            modes_render_1bpp(rgb, glyph, palette[0], palette[1]);
            rgb += 8;
            fill_cols -= 8;
            col += 8;
            tile_mem = mode2_get_glyph_tile_mem(config, 1, tile_size, col, row, row_data, &index);
            glyph = xram[tile_mem + index];
        }
        col += fill_cols;
        if (fill_cols >= 1)
            *rgb++ = palette[(glyph & 0x80) >> 7];
        if (fill_cols >= 2)
            *rgb++ = palette[(glyph & 0x40) >> 6];
        if (fill_cols >= 3)
            *rgb++ = palette[(glyph & 0x20) >> 5];
        if (fill_cols >= 4)
            *rgb++ = palette[(glyph & 0x10) >> 4];
        if (fill_cols >= 5)
            *rgb++ = palette[(glyph & 0x08) >> 3];
        if (fill_cols >= 6)
            *rgb++ = palette[(glyph & 0x04) >> 2];
        if (fill_cols >= 7)
            *rgb++ = palette[(glyph & 0x02) >> 1];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode2_render_1bpp_8x8(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    return mode2_render_1bpp(scanline_id, width, rgb, config_ptr, 8);
}

static bool __attribute__((optimize("O1")))
mode2_render_1bpp_16x16(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    return mode2_render_1bpp(scanline_id, width, rgb, config_ptr, 16);
}

static inline __attribute__((always_inline)) bool __attribute__((optimize("O1")))
mode2_render_2bpp(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr, int16_t tile_size)
{
    if (config_ptr > 0x10000 - sizeof(mode2_config_t))
        return false;
    mode2_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, sizeof(uint8_t), tile_size, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode2_get_palette(config, 2);
    int16_t col = -config->x_pos_px;

    while (width)
    {
        int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width);
        uint16_t index;
        uint16_t tile_mem = mode2_get_glyph_tile_mem(config, 2, tile_size, col, row, row_data, &index);
        uint8_t glyph = xram[tile_mem + index];
        int16_t part = 4 - (col & 3);
        fill_cols -= part;
        col += part;
        switch (part)
        {
        case 4:
            *rgb++ = palette[(glyph & 0xC0) >> 6];
        case 3:
            *rgb++ = palette[(glyph & 0x30) >> 4];
        case 2:
            *rgb++ = palette[(glyph & 0x0C) >> 2];
        case 1:
            *rgb++ = palette[glyph & 0x03];
            if (++index == tile_size / 4)
                tile_mem = mode2_get_glyph_tile_mem(config, 2, tile_size, col, row, row_data, &index);
            glyph = xram[tile_mem + index];
        }
        while (fill_cols > 3)
        {
            *rgb++ = palette[(glyph & 0xC0) >> 6];
            *rgb++ = palette[(glyph & 0x30) >> 4];
            *rgb++ = palette[(glyph & 0x0C) >> 2];
            *rgb++ = palette[glyph & 0x03];
            fill_cols -= 4;
            col += 4;
            if (++index == tile_size / 4)
                tile_mem = mode2_get_glyph_tile_mem(config, 2, tile_size, col, row, row_data, &index);
            glyph = xram[tile_mem + index];
        }
        col += fill_cols;
        if (fill_cols >= 1)
            *rgb++ = palette[(glyph & 0xC0) >> 6];
        if (fill_cols >= 2)
            *rgb++ = palette[(glyph & 0x30) >> 4];
        if (fill_cols >= 3)
            *rgb++ = palette[(glyph & 0x0C) >> 2];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode2_render_2bpp_8x8(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    return mode2_render_2bpp(scanline_id, width, rgb, config_ptr, 8);
}

static bool __attribute__((optimize("O1")))
mode2_render_2bpp_16x16(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    return mode2_render_2bpp(scanline_id, width, rgb, config_ptr, 16);
}

static inline __attribute__((always_inline)) bool __attribute__((optimize("O1")))
mode2_render_4bpp(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr, int16_t tile_size)
{
    if (config_ptr > 0x10000 - sizeof(mode2_config_t))
        return false;
    mode2_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, sizeof(uint8_t), tile_size, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode2_get_palette(config, 4);
    int16_t col = -config->x_pos_px;

    while (width)
    {
        int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width);
        uint16_t index;
        uint16_t tile_mem = mode2_get_glyph_tile_mem(config, 4, tile_size, col, row, row_data, &index);
        uint8_t glyph = xram[tile_mem + index];
        if (col & 1)
        {
            *rgb++ = palette[glyph & 0xF];
            col++;
            fill_cols--;
            if (++index == tile_size / 2)
                tile_mem = mode2_get_glyph_tile_mem(config, 4, tile_size, col, row, row_data, &index);
            glyph = xram[tile_mem + index];
        }
        while (fill_cols > 1)
        {
            *rgb++ = palette[glyph >> 4];
            *rgb++ = palette[glyph & 0xF];
            fill_cols -= 2;
            col += 2;
            if (++index == tile_size / 2)
                tile_mem = mode2_get_glyph_tile_mem(config, 4, tile_size, col, row, row_data, &index);
            glyph = xram[tile_mem + index];
        }
        col += fill_cols;
        if (fill_cols == 1)
            *rgb++ = palette[glyph >> 4];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode2_render_4bpp_8x8(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    return mode2_render_4bpp(scanline_id, width, rgb, config_ptr, 8);
}

static bool __attribute__((optimize("O1")))
mode2_render_4bpp_16x16(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    return mode2_render_4bpp(scanline_id, width, rgb, config_ptr, 16);
}

static inline __attribute__((always_inline)) bool __attribute__((optimize("O1")))
mode2_render_8bpp(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr, int16_t tile_size)
{
    if (config_ptr > 0x10000 - sizeof(mode2_config_t))
        return false;
    mode2_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, sizeof(uint8_t), tile_size, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode2_get_palette(config, 8);
    int16_t col = -config->x_pos_px;

    while (width)
    {
        int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width);
        uint16_t index;
        uint16_t tile_mem = mode2_get_glyph_tile_mem(config, 8, tile_size, col, row, row_data, &index);
        uint8_t glyph = xram[tile_mem + index];
        while (fill_cols > 0)
        {
            *rgb++ = palette[glyph];
            fill_cols -= 1;
            col += 1;
            if (++index == tile_size / 1)
                tile_mem = mode2_get_glyph_tile_mem(config, 8, tile_size, col, row, row_data, &index);
            glyph = xram[tile_mem + index];
        }
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode2_render_8bpp_8x8(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    return mode2_render_8bpp(scanline_id, width, rgb, config_ptr, 8);
}

static bool __attribute__((optimize("O1")))
mode2_render_8bpp_16x16(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    return mode2_render_8bpp(scanline_id, width, rgb, config_ptr, 16);
}

bool mode2_prog(uint16_t *xregs)
{
    const uint16_t attributes = xregs[2];
    const uint16_t config_ptr = xregs[3];
    const int16_t plane = xregs[4];
    const int16_t scanline_begin = xregs[5];
    const int16_t scanline_end = xregs[6];

    if (config_ptr & 1 ||
        config_ptr > 0x10000 - sizeof(mode2_config_t))
        return false;

    void *render_fn;
    switch (attributes)
    {
    case 0:
        render_fn = mode2_render_1bpp_8x8;
        break;
    case 1:
        render_fn = mode2_render_2bpp_8x8;
        break;
    case 2:
        render_fn = mode2_render_4bpp_8x8;
        break;
    case 3:
        render_fn = mode2_render_8bpp_8x8;
        break;
    case 8:
        render_fn = mode2_render_1bpp_16x16;
        break;
    case 9:
        render_fn = mode2_render_2bpp_16x16;
        break;
    case 10:
        render_fn = mode2_render_4bpp_16x16;
        break;
    case 11:
        render_fn = mode2_render_8bpp_16x16;
        break;
    default:
        return false;
    };

    return vga_prog_fill(plane, scanline_begin, scanline_end, config_ptr, render_fn);
}
