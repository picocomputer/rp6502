/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/mode2.h"
#include "pico/scanvideo.h"
#include "sys/vga.h"
#include "sys/mem.h"
#include "term/color.h"
#include "term/font.h"
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

static inline __attribute__((always_inline)) void __attribute__((optimize("O1")))
render_1bpp(uint16_t *buf, uint8_t bits, uint16_t bg, uint16_t fg)
{
    switch (bits >> 4)
    {
    case 0:
        buf[0] = bg;
        buf[1] = bg;
        buf[2] = bg;
        buf[3] = bg;
        break;
    case 1:
        buf[0] = bg;
        buf[1] = bg;
        buf[2] = bg;
        buf[3] = fg;
        break;
    case 2:
        buf[0] = bg;
        buf[1] = bg;
        buf[2] = fg;
        buf[3] = bg;
        break;
    case 3:
        buf[0] = bg;
        buf[1] = bg;
        buf[2] = fg;
        buf[3] = fg;
        break;
    case 4:
        buf[0] = bg;
        buf[1] = fg;
        buf[2] = bg;
        buf[3] = bg;
        break;
    case 5:
        buf[0] = bg;
        buf[1] = fg;
        buf[2] = bg;
        buf[3] = fg;
        break;
    case 6:
        buf[0] = bg;
        buf[1] = fg;
        buf[2] = fg;
        buf[3] = bg;
        break;
    case 7:
        buf[0] = bg;
        buf[1] = fg;
        buf[2] = fg;
        buf[3] = fg;
        break;
    case 8:
        buf[0] = fg;
        buf[1] = bg;
        buf[2] = bg;
        buf[3] = bg;
        break;
    case 9:
        buf[0] = fg;
        buf[1] = bg;
        buf[2] = bg;
        buf[3] = fg;
        break;
    case 10:
        buf[0] = fg;
        buf[1] = bg;
        buf[2] = fg;
        buf[3] = bg;
        break;
    case 11:
        buf[0] = fg;
        buf[1] = bg;
        buf[2] = fg;
        buf[3] = fg;
        break;
    case 12:
        buf[0] = fg;
        buf[1] = fg;
        buf[2] = bg;
        buf[3] = bg;
        break;
    case 13:
        buf[0] = fg;
        buf[1] = fg;
        buf[2] = bg;
        buf[3] = fg;
        break;
    case 14:
        buf[0] = fg;
        buf[1] = fg;
        buf[2] = fg;
        buf[3] = bg;
        break;
    case 15:
        buf[0] = fg;
        buf[1] = fg;
        buf[2] = fg;
        buf[3] = fg;
        break;
    }
    switch (bits & 0xF)
    {
    case 0:
        buf[4] = bg;
        buf[5] = bg;
        buf[6] = bg;
        buf[7] = bg;
        break;
    case 1:
        buf[4] = bg;
        buf[5] = bg;
        buf[6] = bg;
        buf[7] = fg;
        break;
    case 2:
        buf[4] = bg;
        buf[5] = bg;
        buf[6] = fg;
        buf[7] = bg;
        break;
    case 3:
        buf[4] = bg;
        buf[5] = bg;
        buf[6] = fg;
        buf[7] = fg;
        break;
    case 4:
        buf[4] = bg;
        buf[5] = fg;
        buf[6] = bg;
        buf[7] = bg;
        break;
    case 5:
        buf[4] = bg;
        buf[5] = fg;
        buf[6] = bg;
        buf[7] = fg;
        break;
    case 6:
        buf[4] = bg;
        buf[5] = fg;
        buf[6] = fg;
        buf[7] = bg;
        break;
    case 7:
        buf[4] = bg;
        buf[5] = fg;
        buf[6] = fg;
        buf[7] = fg;
        break;
    case 8:
        buf[4] = fg;
        buf[5] = bg;
        buf[6] = bg;
        buf[7] = bg;
        break;
    case 9:
        buf[4] = fg;
        buf[5] = bg;
        buf[6] = bg;
        buf[7] = fg;
        break;
    case 10:
        buf[4] = fg;
        buf[5] = bg;
        buf[6] = fg;
        buf[7] = bg;
        break;
    case 11:
        buf[4] = fg;
        buf[5] = bg;
        buf[6] = fg;
        buf[7] = fg;
        break;
    case 12:
        buf[4] = fg;
        buf[5] = fg;
        buf[6] = bg;
        buf[7] = bg;
        break;
    case 13:
        buf[4] = fg;
        buf[5] = fg;
        buf[6] = bg;
        buf[7] = fg;
        break;
    case 14:
        buf[4] = fg;
        buf[5] = fg;
        buf[6] = fg;
        buf[7] = bg;
        break;
    case 15:
        buf[4] = fg;
        buf[5] = fg;
        buf[6] = fg;
        buf[7] = fg;
        break;
    }
}

static inline volatile const uint8_t *__attribute__((optimize("O1")))
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

static inline volatile const uint16_t *__attribute__((optimize("O1")))
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

// TODO Not 16bpp
static inline __attribute__((always_inline)) uint8_t __attribute__((optimize("O1")))
mode2_get_glyph_data(mode2_config_t *config, int16_t bpp, int16_t tile_size, int16_t col, int16_t row, volatile const uint8_t *row_data)
{
    uint32_t row_size = tile_size == 8 ? bpp : 2 * bpp;
    uint32_t mem_size = row_size * tile_size;
    uint8_t index = (col / 8) & (row_size - 1);
    uint8_t tile_id = row_data[col / (tile_size / bpp)];
    uint16_t tile_mem = (uint32_t)config->xram_tile_ptr + mem_size * tile_id + row_size * row + index;
    return xram[tile_mem];
}

static bool __attribute__((optimize("O1")))
mode2_render_1bpp_8x8(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    if (config_ptr > 0x10000 - sizeof(mode2_config_t))
        return false;
    mode2_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, sizeof(uint8_t), 8, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode2_get_palette(config, 1);
    int16_t col = -config->x_pos_px;

    while (width)
    {
        int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width);
        uint8_t glyph = mode2_get_glyph_data(config, 1, 8, col, row, row_data);
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
            glyph = mode2_get_glyph_data(config, 1, 8, col, row, row_data);
        }
        while (fill_cols > 7)
        {
            render_1bpp(rgb, glyph, palette[0], palette[1]);
            rgb += 8;
            fill_cols -= 8;
            col += 8;
            glyph = mode2_get_glyph_data(config, 1, 8, col, row, row_data);
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
mode2_render_1bpp_16x16(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    if (config_ptr > 0x10000 - sizeof(mode2_config_t))
        return false;
    mode2_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, sizeof(uint8_t), 16, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode2_get_palette(config, 1);
    int16_t col = -config->x_pos_px;

    while (width)
    {
        int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width);
        uint8_t glyph = mode2_get_glyph_data(config, 1, 16, col, row, row_data);
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
            glyph = mode2_get_glyph_data(config, 1, 16, col, row, row_data);
        }
        while (fill_cols > 7)
        {
            render_1bpp(rgb, glyph, palette[0], palette[1]);
            rgb += 8;
            fill_cols -= 8;
            col += 8;
            glyph = mode2_get_glyph_data(config, 1, 16, col, row, row_data);
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
    // case 1:
    //     render_fn = mode2_render_2bpp_8x8;
    //     break;
    // case 2:
    //     render_fn = mode2_render_4bpp_8x8;
    //     break;
    // case 3:
    //     render_fn = mode2_render_8bpp_8x8;
    //     break;
    // case 4:
    //     render_fn = mode2_render_16bpp_8x8;
    //     break;
    case 8:
        render_fn = mode2_render_1bpp_16x16;
        break;
    // case 9:
    //     render_fn = mode2_render_2bpp_16x16;
    //     break;
    // case 10:
    //     render_fn = mode2_render_4bpp_16x16;
    //     break;
    // case 11:
    //     render_fn = mode2_render_8bpp_16x16;
    //     break;
    // case 12:
    //     render_fn = mode2_render_16bpp_16x16;
    //     break;
    default:
        return false;
    };

    return vga_prog_fill(plane, scanline_begin, scanline_end, config_ptr, render_fn);
}
