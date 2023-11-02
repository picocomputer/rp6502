/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/mode1.h"
#include "modes/modes.h"
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
    int16_t width_chars;
    int16_t height_chars;
    uint16_t xram_data_ptr;
    uint16_t xram_palette_ptr;
    uint16_t xram_font_ptr;
} mode1_config_t;

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

static volatile const uint8_t *__attribute__((optimize("O1")))
mode1_scanline_to_data(int16_t scanline_id, mode1_config_t *config, size_t cell_size, int16_t font_height, int16_t *row)
{
    *row = scanline_id - config->y_pos_px;
    const int16_t height = config->height_chars * font_height;
    if (config->y_wrap)
    {
        if (*row < 0)
            *row += (-(*row + 1) / height + 1) * height;
        if (*row >= height)
            *row -= ((*row - height) / height + 1) * height;
    }
    if (*row < 0 || *row >= height || config->width_chars < 1 || height < 1)
        return NULL;
    const int32_t sizeof_row = (int32_t)config->width_chars * cell_size;
    const int32_t sizeof_bitmap = (int32_t)config->height_chars * sizeof_row;
    if (sizeof_bitmap > 0x10000 - config->xram_data_ptr)
        return NULL;
    return &xram[config->xram_data_ptr + *row / font_height * sizeof_row];
}

static volatile const uint16_t *__attribute__((optimize("O1")))
mode1_get_palette(mode1_config_t *config, int16_t bpp)
{
    if (!(config->xram_palette_ptr & 1) &&
        config->xram_palette_ptr <= 0x10000 - sizeof(uint16_t) * (2 ^ bpp))
        return (uint16_t *)&xram[config->xram_palette_ptr];
    if (bpp == 1)
        return color_2;
    return color_256;
}

static volatile const uint8_t *__attribute__((optimize("O1")))
mode1_get_font(mode1_config_t *config, int16_t font_height)
{
    if (config->xram_font_ptr <= 0x10000 - 256 * font_height)
        return &xram[config->xram_font_ptr];
    if (font_height == 8)
        return font8;
    return font16;
}

static inline __attribute__((always_inline)) int16_t __attribute__((optimize("O1")))
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

static bool __attribute__((optimize("O1")))
mode1_render_1bpp(int16_t scanline_id, int16_t width, uint16_t *rgb,
                  uint16_t config_ptr, int16_t font_height)
{
    if (config_ptr > 0x10000 - sizeof(mode1_config_t))
        return false;
    mode1_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const mode1_1bpp_data_t *row_data =
        (void *)mode1_scanline_to_data(scanline_id, config, sizeof(mode1_1bpp_data_t), font_height, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode1_get_palette(config, 1);
    volatile const uint8_t *font = mode1_get_font(config, font_height) + 256 * (row & (font_height - 1));
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode1_fill_cols(config, &rgb, &col, &width);
        volatile const mode1_1bpp_data_t *data = &row_data[col / 8];
        uint8_t glyph = font[data->glyph_code];
        int16_t part = 8 - (col & 7);
        if (part > config->width_chars * 8 - col)
            part = config->width_chars * 8 - col;
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
            glyph = font[(++data)->glyph_code];
        }
        col += fill_cols;
        while (fill_cols > 7)
        {
            modes_render_1bpp(rgb, glyph, palette[0], palette[1]);
            rgb += 8;
            fill_cols -= 8;
            glyph = font[(++data)->glyph_code];
        }
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
mode1_render_1bpp_8x8(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    mode1_render_1bpp(scanline_id, width, rgb, config_ptr, 8);
}

static bool __attribute__((optimize("O1")))
mode1_render_1bpp_8x16(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    mode1_render_1bpp(scanline_id, width, rgb, config_ptr, 16);
}

static bool __attribute__((optimize("O1")))
mode1_render_4bpp(int16_t scanline_id, int16_t width, uint16_t *rgb,
                  uint16_t config_ptr, int16_t font_height)
{
    if (config_ptr > 0x10000 - sizeof(mode1_config_t))
        return false;
    mode1_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const mode1_4bpp_data_t *row_data =
        (void *)mode1_scanline_to_data(scanline_id, config, sizeof(mode1_4bpp_data_t), font_height, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode1_get_palette(config, 4);
    volatile const uint8_t *font = mode1_get_font(config, font_height) + 256 * (row & (font_height - 1));
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode1_fill_cols(config, &rgb, &col, &width);
        volatile const mode1_4bpp_data_t *data = &row_data[col / 8];
        uint8_t glyph = font[data->glyph_code];
        uint16_t colors[2] = {
            palette[data->bg_fg_index >> 4],
            palette[data->bg_fg_index & 0xF]};
        int16_t part = 8 - (col & 7);
        if (part > config->width_chars * 8 - col)
            part = config->width_chars * 8 - col;
        fill_cols -= part;
        col += part;
        switch (part)
        {
        case 8:
            *rgb++ = colors[(glyph & 0x80) >> 7];
        case 7:
            *rgb++ = colors[(glyph & 0x40) >> 6];
        case 6:
            *rgb++ = colors[(glyph & 0x20) >> 5];
        case 5:
            *rgb++ = colors[(glyph & 0x10) >> 4];
        case 4:
            *rgb++ = colors[(glyph & 0x08) >> 3];
        case 3:
            *rgb++ = colors[(glyph & 0x04) >> 2];
        case 2:
            *rgb++ = colors[(glyph & 0x02) >> 1];
        case 1:
            *rgb++ = colors[glyph & 0x01];
            glyph = font[(++data)->glyph_code];
        }
        col += fill_cols;
        while (fill_cols > 7)
        {
            modes_render_1bpp(rgb, glyph, palette[data->bg_fg_index >> 4], palette[data->bg_fg_index & 0xF]);
            rgb += 8;
            fill_cols -= 8;
            glyph = font[(++data)->glyph_code];
        }
        colors[0] = palette[data->bg_fg_index >> 4];
        colors[1] = palette[data->bg_fg_index & 0xF];
        if (fill_cols >= 1)
            *rgb++ = colors[(glyph & 0x80) >> 7];
        if (fill_cols >= 2)
            *rgb++ = colors[(glyph & 0x40) >> 6];
        if (fill_cols >= 3)
            *rgb++ = colors[(glyph & 0x20) >> 5];
        if (fill_cols >= 4)
            *rgb++ = colors[(glyph & 0x10) >> 4];
        if (fill_cols >= 5)
            *rgb++ = colors[(glyph & 0x08) >> 3];
        if (fill_cols >= 6)
            *rgb++ = colors[(glyph & 0x04) >> 2];
        if (fill_cols >= 7)
            *rgb++ = colors[(glyph & 0x02) >> 1];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode1_render_4bpp_8x8(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    mode1_render_4bpp(scanline_id, width, rgb, config_ptr, 8);
}

static bool __attribute__((optimize("O1")))
mode1_render_4bpp_8x16(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    mode1_render_4bpp(scanline_id, width, rgb, config_ptr, 16);
}

static bool __attribute__((optimize("O1")))
mode1_render_4bppr(int16_t scanline_id, int16_t width, uint16_t *rgb,
                   uint16_t config_ptr, int16_t font_height)
{
    if (config_ptr > 0x10000 - sizeof(mode1_config_t))
        return false;
    mode1_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const mode1_4bppr_data_t *row_data =
        (void *)mode1_scanline_to_data(scanline_id, config, sizeof(mode1_4bppr_data_t), font_height, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode1_get_palette(config, 4);
    volatile const uint8_t *font = mode1_get_font(config, font_height) + 256 * (row & (font_height - 1));
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode1_fill_cols(config, &rgb, &col, &width);
        volatile const mode1_4bppr_data_t *data = &row_data[col / 8];
        uint8_t glyph = font[data->glyph_code];
        uint16_t colors[2] = {
            palette[data->fg_bg_index & 0xF],
            palette[data->fg_bg_index >> 4]};
        int16_t part = 8 - (col & 7);
        if (part > config->width_chars * 8 - col)
            part = config->width_chars * 8 - col;
        fill_cols -= part;
        col += part;
        switch (part)
        {
        case 8:
            *rgb++ = colors[(glyph & 0x80) >> 7];
        case 7:
            *rgb++ = colors[(glyph & 0x40) >> 6];
        case 6:
            *rgb++ = colors[(glyph & 0x20) >> 5];
        case 5:
            *rgb++ = colors[(glyph & 0x10) >> 4];
        case 4:
            *rgb++ = colors[(glyph & 0x08) >> 3];
        case 3:
            *rgb++ = colors[(glyph & 0x04) >> 2];
        case 2:
            *rgb++ = colors[(glyph & 0x02) >> 1];
        case 1:
            *rgb++ = colors[glyph & 0x01];
            glyph = font[(++data)->glyph_code];
        }
        col += fill_cols;
        while (fill_cols > 7)
        {
            modes_render_1bpp(rgb, glyph, palette[data->fg_bg_index & 0xF], palette[data->fg_bg_index >> 4]);
            rgb += 8;
            fill_cols -= 8;
            glyph = font[(++data)->glyph_code];
        }
        colors[0] = palette[data->fg_bg_index & 0xF];
        colors[1] = palette[data->fg_bg_index >> 4];
        if (fill_cols >= 1)
            *rgb++ = colors[(glyph & 0x80) >> 7];
        if (fill_cols >= 2)
            *rgb++ = colors[(glyph & 0x40) >> 6];
        if (fill_cols >= 3)
            *rgb++ = colors[(glyph & 0x20) >> 5];
        if (fill_cols >= 4)
            *rgb++ = colors[(glyph & 0x10) >> 4];
        if (fill_cols >= 5)
            *rgb++ = colors[(glyph & 0x08) >> 3];
        if (fill_cols >= 6)
            *rgb++ = colors[(glyph & 0x04) >> 2];
        if (fill_cols >= 7)
            *rgb++ = colors[(glyph & 0x02) >> 1];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode1_render_4bppr_8x8(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    mode1_render_4bppr(scanline_id, width, rgb, config_ptr, 8);
}

static bool __attribute__((optimize("O1")))
mode1_render_4bppr_8x16(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    mode1_render_4bppr(scanline_id, width, rgb, config_ptr, 16);
}

static bool __attribute__((optimize("O1")))
mode1_render_8bpp(int16_t scanline_id, int16_t width, uint16_t *rgb,
                  uint16_t config_ptr, int16_t font_height)
{
    if (config_ptr > 0x10000 - sizeof(mode1_config_t))
        return false;
    mode1_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const mode1_8bpp_data_t *row_data =
        (void *)mode1_scanline_to_data(scanline_id, config, sizeof(mode1_8bpp_data_t), font_height, &row);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode1_get_palette(config, 8);
    volatile const uint8_t *font = mode1_get_font(config, font_height) + 256 * (row & (font_height - 1));
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode1_fill_cols(config, &rgb, &col, &width);
        volatile const mode1_8bpp_data_t *data = &row_data[col / 8];
        uint8_t glyph = font[data->glyph_code];
        uint16_t colors[2] = {
            palette[data->bg_index],
            palette[data->fg_index]};
        int16_t part = 8 - (col & 7);
        if (part > config->width_chars * 8 - col)
            part = config->width_chars * 8 - col;
        fill_cols -= part;
        col += part;
        switch (part)
        {
        case 8:
            *rgb++ = colors[(glyph & 0x80) >> 7];
        case 7:
            *rgb++ = colors[(glyph & 0x40) >> 6];
        case 6:
            *rgb++ = colors[(glyph & 0x20) >> 5];
        case 5:
            *rgb++ = colors[(glyph & 0x10) >> 4];
        case 4:
            *rgb++ = colors[(glyph & 0x08) >> 3];
        case 3:
            *rgb++ = colors[(glyph & 0x04) >> 2];
        case 2:
            *rgb++ = colors[(glyph & 0x02) >> 1];
        case 1:
            *rgb++ = colors[glyph & 0x01];
            glyph = font[(++data)->glyph_code];
        }
        col += fill_cols;
        while (fill_cols > 7)
        {
            modes_render_1bpp(rgb, glyph, palette[data->bg_index], palette[data->fg_index]);
            rgb += 8;
            fill_cols -= 8;
            glyph = font[(++data)->glyph_code];
        }
        colors[0] = palette[data->bg_index];
        colors[1] = palette[data->fg_index];
        if (fill_cols >= 1)
            *rgb++ = colors[(glyph & 0x80) >> 7];
        if (fill_cols >= 2)
            *rgb++ = colors[(glyph & 0x40) >> 6];
        if (fill_cols >= 3)
            *rgb++ = colors[(glyph & 0x20) >> 5];
        if (fill_cols >= 4)
            *rgb++ = colors[(glyph & 0x10) >> 4];
        if (fill_cols >= 5)
            *rgb++ = colors[(glyph & 0x08) >> 3];
        if (fill_cols >= 6)
            *rgb++ = colors[(glyph & 0x04) >> 2];
        if (fill_cols >= 7)
            *rgb++ = colors[(glyph & 0x02) >> 1];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode1_render_8bpp_8x8(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    mode1_render_8bpp(scanline_id, width, rgb, config_ptr, 8);
}

static bool __attribute__((optimize("O1")))
mode1_render_8bpp_8x16(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    mode1_render_8bpp(scanline_id, width, rgb, config_ptr, 16);
}

static bool __attribute__((optimize("O1")))
mode1_render_16bpp(int16_t scanline_id, int16_t width, uint16_t *rgb,
                   uint16_t config_ptr, int16_t font_height)
{
    if (config_ptr > 0x10000 - sizeof(mode1_config_t))
        return false;
    mode1_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    volatile const mode1_16bpp_data_t *row_data =
        (void *)mode1_scanline_to_data(scanline_id, config, sizeof(mode1_16bpp_data_t), font_height, &row);
    if (!row_data)
        return false;
    volatile const uint8_t *font = mode1_get_font(config, font_height) + 256 * (row & (font_height - 1));
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode1_fill_cols(config, &rgb, &col, &width);
        volatile const mode1_16bpp_data_t *data = &row_data[col / 8];
        uint8_t glyph = font[data->glyph_code];
        uint16_t colors[2] = {data->bg_color, data->fg_color};
        int16_t part = 8 - (col & 7);
        if (part > config->width_chars * 8 - col)
            part = config->width_chars * 8 - col;
        fill_cols -= part;
        col += part;
        switch (part)
        {
        case 8:
            *rgb++ = colors[(glyph & 0x80) >> 7];
        case 7:
            *rgb++ = colors[(glyph & 0x40) >> 6];
        case 6:
            *rgb++ = colors[(glyph & 0x20) >> 5];
        case 5:
            *rgb++ = colors[(glyph & 0x10) >> 4];
        case 4:
            *rgb++ = colors[(glyph & 0x08) >> 3];
        case 3:
            *rgb++ = colors[(glyph & 0x04) >> 2];
        case 2:
            *rgb++ = colors[(glyph & 0x02) >> 1];
        case 1:
            *rgb++ = colors[glyph & 0x01];
            glyph = font[(++data)->glyph_code];
        }
        col += fill_cols;
        while (fill_cols > 7)
        {
            modes_render_1bpp(rgb, glyph, data->bg_color, data->fg_color);
            rgb += 8;
            fill_cols -= 8;
            glyph = font[(++data)->glyph_code];
        }
        colors[0] = data->bg_color;
        colors[1] = data->fg_color;
        if (fill_cols >= 1)
            *rgb++ = colors[(glyph & 0x80) >> 7];
        if (fill_cols >= 2)
            *rgb++ = colors[(glyph & 0x40) >> 6];
        if (fill_cols >= 3)
            *rgb++ = colors[(glyph & 0x20) >> 5];
        if (fill_cols >= 4)
            *rgb++ = colors[(glyph & 0x10) >> 4];
        if (fill_cols >= 5)
            *rgb++ = colors[(glyph & 0x08) >> 3];
        if (fill_cols >= 6)
            *rgb++ = colors[(glyph & 0x04) >> 2];
        if (fill_cols >= 7)
            *rgb++ = colors[(glyph & 0x02) >> 1];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode1_render_16bpp_8x8(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    mode1_render_16bpp(scanline_id, width, rgb, config_ptr, 8);
}

static bool __attribute__((optimize("O1")))
mode1_render_16bpp_8x16(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    mode1_render_16bpp(scanline_id, width, rgb, config_ptr, 16);
}

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

    void *render_fn;
    switch (attributes)
    {
    case 0:
        render_fn = mode1_render_1bpp_8x8;
        break;
    case 1:
        render_fn = mode1_render_4bppr_8x8;
        break;
    case 2:
        render_fn = mode1_render_4bpp_8x8;
        break;
    case 3:
        render_fn = mode1_render_8bpp_8x8;
        break;
    case 4:
        render_fn = mode1_render_16bpp_8x8;
        break;
    case 8:
        render_fn = mode1_render_1bpp_8x16;
        break;
    case 9:
        render_fn = mode1_render_4bppr_8x16;
        break;
    case 10:
        render_fn = mode1_render_4bpp_8x16;
        break;
    case 11:
        render_fn = mode1_render_8bpp_8x16;
        break;
    case 12:
        render_fn = mode1_render_16bpp_8x16;
        break;
    default:
        return false;
    };

    return vga_prog_fill(plane, scanline_begin, scanline_end, config_ptr, render_fn);
}
