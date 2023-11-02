/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/mode3.h"
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
    int16_t width_px;
    int16_t height_px;
    uint16_t xram_data_ptr;
    uint16_t xram_palette_ptr;
} mode3_config_t;

static volatile const uint8_t *__attribute__((optimize("O1")))
mode3_scanline_to_data(int16_t scanline_id, mode3_config_t *config, int16_t bpp)
{
    int16_t row = scanline_id - config->y_pos_px;
    const int16_t height = config->height_px;
    if (config->y_wrap)
    {
        if (row < 0)
            row += (-(row + 1) / height + 1) * height;
        if (row >= height)
            row -= ((row - height) / height + 1) * height;
    }
    if (row < 0 || row >= height || config->width_px < 1 || height < 1)
        return NULL;
    const int32_t sizeof_row = ((int32_t)config->width_px * bpp + 7) / 8;
    const int32_t sizeof_bitmap = (int32_t)height * sizeof_row;
    if (sizeof_bitmap > 0x10000 - config->xram_data_ptr)
        return NULL;
    return &xram[config->xram_data_ptr + row * sizeof_row];
}

static volatile const uint16_t *__attribute__((optimize("O1")))
mode3_get_palette(mode3_config_t *config, int16_t bpp)
{
    if (!(config->xram_palette_ptr & 1) &&
        config->xram_palette_ptr <= 0x10000 - sizeof(uint16_t) * (2 ^ bpp))
        return (uint16_t *)&xram[config->xram_palette_ptr];
    if (bpp == 1)
        return color_2;
    return color_256;
}

static inline __attribute__((always_inline)) int16_t __attribute__((optimize("O1")))
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

static bool __attribute__((optimize("O1")))
mode3_render_1bpp_0r(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    if (config_ptr > 0x10000 - sizeof(mode3_config_t))
        return false;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 1);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 1);
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col / 8];
        int16_t part = 8 - (col & 7);
        if (part > config->width_px - col)
            part = config->width_px - col;
        fill_cols -= part;
        col += part;
        switch (part)
        {
        case 8:
            *rgb++ = palette[(*data & 0x80) >> 7];
        case 7:
            *rgb++ = palette[(*data & 0x40) >> 6];
        case 6:
            *rgb++ = palette[(*data & 0x20) >> 5];
        case 5:
            *rgb++ = palette[(*data & 0x10) >> 4];
        case 4:
            *rgb++ = palette[(*data & 0x08) >> 3];
        case 3:
            *rgb++ = palette[(*data & 0x04) >> 2];
        case 2:
            *rgb++ = palette[(*data & 0x02) >> 1];
        case 1:
            *rgb++ = palette[*data++ & 0x01];
        }
        col += fill_cols;
        while (fill_cols > 7)
        {
            modes_render_1bpp(rgb, *data++, palette[0], palette[1]);
            rgb += 8;
            fill_cols -= 8;
        }
        if (fill_cols >= 1)
            *rgb++ = palette[(*data & 0x80) >> 7];
        if (fill_cols >= 2)
            *rgb++ = palette[(*data & 0x40) >> 6];
        if (fill_cols >= 3)
            *rgb++ = palette[(*data & 0x20) >> 5];
        if (fill_cols >= 4)
            *rgb++ = palette[(*data & 0x10) >> 4];
        if (fill_cols >= 5)
            *rgb++ = palette[(*data & 0x08) >> 3];
        if (fill_cols >= 6)
            *rgb++ = palette[(*data & 0x04) >> 2];
        if (fill_cols >= 7)
            *rgb++ = palette[(*data & 0x02) >> 1];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode3_render_1bpp_1r(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    if (config_ptr > 0x10000 - sizeof(mode3_config_t))
        return false;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 1);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 1);
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col / 8];
        int16_t part = 8 - (col & 7);
        if (part > config->width_px - col)
            part = config->width_px - col;
        fill_cols -= part;
        col += part;
        switch (part)
        {
        case 8:
            *rgb++ = palette[*data & 0x01];
        case 7:
            *rgb++ = palette[(*data & 0x02) >> 1];
        case 6:
            *rgb++ = palette[(*data & 0x04) >> 2];
        case 5:
            *rgb++ = palette[(*data & 0x08) >> 3];
        case 4:
            *rgb++ = palette[(*data & 0x10) >> 4];
        case 3:
            *rgb++ = palette[(*data & 0x20) >> 5];
        case 2:
            *rgb++ = palette[(*data & 0x40) >> 6];
        case 1:
            *rgb++ = palette[(*data++ & 0x80) >> 7];
        }
        col += fill_cols;
        while (fill_cols > 7)
        {
            modes_render_1bpp_reverse(rgb, *data++, palette[0], palette[1]);
            rgb += 8;
            fill_cols -= 8;
        }
        if (fill_cols >= 1)
            *rgb++ = palette[*data & 0x01];
        if (fill_cols >= 2)
            *rgb++ = palette[(*data & 0x02) >> 1];
        if (fill_cols >= 3)
            *rgb++ = palette[(*data & 0x04) >> 2];
        if (fill_cols >= 4)
            *rgb++ = palette[(*data & 0x08) >> 3];
        if (fill_cols >= 5)
            *rgb++ = palette[(*data & 0x10) >> 4];
        if (fill_cols >= 6)
            *rgb++ = palette[(*data & 0x20) >> 5];
        if (fill_cols >= 7)
            *rgb++ = palette[(*data & 0x40) >> 6];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode3_render_2bpp_0r(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    if (config_ptr > 0x10000 - sizeof(mode3_config_t))
        return false;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 2);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 2);
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col / 4];
        int16_t part = 4 - (col & 3);
        if (part > config->width_px - col)
            part = config->width_px - col;
        fill_cols -= part;
        col += part;
        switch (part)
        {
        case 4:
            *rgb++ = palette[(*data & 0xC0) >> 6];
        case 3:
            *rgb++ = palette[(*data & 0x30) >> 4];
        case 2:
            *rgb++ = palette[(*data & 0x0C) >> 2];
        case 1:
            *rgb++ = palette[*data++ & 0x03];
        }
        col += fill_cols;
        while (fill_cols > 3)
        {
            *rgb++ = palette[(*data & 0xC0) >> 6];
            *rgb++ = palette[(*data & 0x30) >> 4];
            *rgb++ = palette[(*data & 0x0C) >> 2];
            *rgb++ = palette[*data++ & 0x03];
            fill_cols -= 4;
        }
        if (fill_cols >= 1)
            *rgb++ = palette[(*data & 0xC0) >> 6];
        if (fill_cols >= 2)
            *rgb++ = palette[(*data & 0x30) >> 4];
        if (fill_cols >= 3)
            *rgb++ = palette[(*data & 0x0C) >> 2];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode3_render_2bpp_1r(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    if (config_ptr > 0x10000 - sizeof(mode3_config_t))
        return false;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 2);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 2);
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col / 4];
        int16_t part = 4 - (col & 3);
        if (part > config->width_px - col)
            part = config->width_px - col;
        fill_cols -= part;
        col += part;
        switch (part)
        {
        case 4:
            *rgb++ = palette[*data & 0x03];
        case 3:
            *rgb++ = palette[(*data & 0x0C) >> 2];
        case 2:
            *rgb++ = palette[(*data & 0x30) >> 4];
        case 1:
            *rgb++ = palette[(*data++ & 0xC0) >> 6];
        }
        col += fill_cols;
        while (fill_cols > 3)
        {
            *rgb++ = palette[*data & 0x03];
            *rgb++ = palette[(*data & 0x0C) >> 2];
            *rgb++ = palette[(*data & 0x30) >> 4];
            *rgb++ = palette[(*data++ & 0xC0) >> 6];
            fill_cols -= 4;
        }
        if (fill_cols >= 1)
            *rgb++ = palette[*data & 0x03];
        if (fill_cols >= 2)
            *rgb++ = palette[(*data & 0x0C) >> 2];
        if (fill_cols >= 3)
            *rgb++ = palette[(*data & 0x30) >> 4];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode3_render_4bpp_0r(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    if (config_ptr > 0x10000 - sizeof(mode3_config_t))
        return false;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 4);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 4);
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col / 2];
        if (col & 1)
        {
            *rgb++ = palette[*data++ & 0xF];
            col++;
            fill_cols--;
        }
        col += fill_cols;
        while (fill_cols > 1)
        {
            *rgb++ = palette[*data >> 4];
            *rgb++ = palette[*data++ & 0xF];
            fill_cols -= 2;
        }
        if (fill_cols == 1)
            *rgb++ = palette[*data >> 4];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode3_render_4bpp_1r(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    if (config_ptr > 0x10000 - sizeof(mode3_config_t))
        return false;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 4);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 4);
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col / 2];
        if (col & 1)
        {
            *rgb++ = palette[*data++ >> 4];
            col++;
            fill_cols--;
        }
        col += fill_cols;
        while (fill_cols > 1)
        {
            *rgb++ = palette[*data & 0xF];
            *rgb++ = palette[*data++ >> 4];
            fill_cols -= 2;
        }
        if (fill_cols == 1)
            *rgb++ = palette[*data & 0xF];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode3_render_8bpp(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    if (config_ptr > 0x10000 - sizeof(mode3_config_t))
        return false;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 8);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 8);
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col];
        col += fill_cols;
        for (; fill_cols; fill_cols--)
            *rgb++ = palette[*data++];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode3_render_16bpp(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    if (config_ptr > 0x10000 - sizeof(mode3_config_t))
        return false;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint16_t *row_data = (uint16_t *)mode3_scanline_to_data(scanline_id, config, 16);
    if (!row_data || (uint32_t)row_data & 1)
        return false;
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint16_t *data = &row_data[col];
        col += fill_cols;
        for (; fill_cols; fill_cols--)
            *rgb++ = *data++;
    }
    return true;
}

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

    void *render_fn;
    switch (attributes)
    {
    case 0:
        render_fn = mode3_render_1bpp_0r;
        break;
    case 1:
        render_fn = mode3_render_2bpp_0r;
        break;
    case 2:
        render_fn = mode3_render_4bpp_0r;
        break;
    case 3:
        render_fn = mode3_render_8bpp;
        break;
    case 4:
        render_fn = mode3_render_16bpp;
        break;
    case 8:
        render_fn = mode3_render_1bpp_1r;
        break;
    case 9:
        render_fn = mode3_render_2bpp_1r;
        break;
    case 10:
        render_fn = mode3_render_4bpp_1r;
        break;
    default:
        return false;
    };

    return vga_prog_fill(plane, scanline_begin, scanline_end, config_ptr, render_fn);
}
