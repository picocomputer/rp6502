/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/mode3.h"
#include "sys/vga.h"
#include "sys/xram.h"
#include "term/color.h"
#include "pico/scanvideo.h"
#include <string.h>

typedef struct
{
    int16_t xpos_px;
    int16_t ypos_px;
    int16_t width_px;
    int16_t height_px;
    uint16_t xram_data_ptr;
    uint16_t xram_palette_ptr;
} mode3_config_t;

typedef struct
{
    uint8_t font_code;
    uint8_t attributes;
    uint16_t fg_color;
    uint16_t bg_color;
} mode3_16bpp_data_t;

static int16_t __attribute__((optimize("O1")))
mode3_scanline_to_row(int16_t scanline_id, mode3_config_t *config, bool y_wrap)
{
    int16_t row = scanline_id - config->ypos_px;
    int16_t height = config->height_px;
    if (y_wrap)
    {
        if (row < 0)
            row += (-(row + 1) / height + 1) * height;
        if (row >= height)
            row -= ((row - height) / height + 1) * height;
    }
    if (row >= height)
        row = -1;
    return row;
}

volatile const uint8_t *__attribute__((optimize("O1")))
mode3_row_to_data(int16_t row, mode3_config_t *config, int16_t bpp)
{
    if (row < 0 || config->width_px < 1 || config->height_px < 1)
        return NULL;
    const int32_t sizeof_row = ((int32_t)config->width_px * bpp + 7) / 8;
    const int32_t sizeof_bitmap = (int32_t)config->height_px * sizeof_row;
    if (sizeof_bitmap > 0x10000 - config->xram_data_ptr)
        return NULL;
    return &xram[config->xram_data_ptr + row * sizeof_row];
}

volatile const uint16_t *__attribute__((optimize("O1")))
mode3_get_palette(mode3_config_t *config, int16_t bpp)
{
    if (config->xram_palette_ptr <= 0x10000 - sizeof(uint16_t) * (2 ^ bpp))
        return (uint16_t *)&xram[config->xram_palette_ptr];
    if (bpp == 1)
        return color_2;
    return color_256;
}

static bool __attribute__((optimize("O1")))
mode3_render_4bpp_0r(int16_t row, int16_t width, uint16_t *rgb, mode3_config_t *config, bool x_wrap)
{
    volatile const uint8_t *row_data = mode3_row_to_data(row, config, 4);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 4);
    int16_t col = -config->xpos_px;
    while (width)
    {
        if (col < 0)
        {
            if (x_wrap)
                col += (-(col + 1) / config->width_px + 1) * config->width_px;
            else
            {
                uint16_t empty_cols = -col;
                if (empty_cols > width)
                    empty_cols = width;
                memset(rgb, 0, sizeof(uint16_t) * empty_cols);
                col += empty_cols;
                rgb += empty_cols;
                width -= empty_cols;
                continue;
            }
        }
        if (col >= config->width_px)
        {
            if (x_wrap)
                col -= ((col - config->width_px) / config->width_px + 1) * config->width_px;
            else
            {
                memset(rgb, 0, sizeof(uint16_t) * width);
                break;
            }
        }
        int16_t fill_cols = width;
        if (fill_cols > config->width_px - col)
            fill_cols = config->width_px - col;
        width -= fill_cols;
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
mode3_render_4bpp_00xy_0r(int16_t scanline_id, int16_t width, uint16_t *rgb, void *config)
{
    int16_t row = mode3_scanline_to_row(scanline_id, config, false);
    return mode3_render_4bpp_0r(row, width, rgb, config, false);
}

static bool __attribute__((optimize("O1")))
mode3_render_4bpp_10xy_0r(int16_t scanline_id, int16_t width, uint16_t *rgb, void *config)
{
    int16_t row = mode3_scanline_to_row(scanline_id, config, false);
    return mode3_render_4bpp_0r(row, width, rgb, config, true);
}

static bool __attribute__((optimize("O1")))
mode3_render_4bpp_01xy_0r(int16_t scanline_id, int16_t width, uint16_t *rgb, void *config)
{
    int16_t row = mode3_scanline_to_row(scanline_id, config, true);
    return mode3_render_4bpp_0r(row, width, rgb, config, false);
}

static bool __attribute__((optimize("O1")))
mode3_render_4bpp_11xy_0r(int16_t scanline_id, int16_t width, uint16_t *rgb, void *config)
{
    int16_t row = mode3_scanline_to_row(scanline_id, config, true);
    return mode3_render_4bpp_0r(row, width, rgb, config, true);
}

static bool __attribute__((optimize("O1")))
mode3_render_4bpp_1r(int16_t row, int16_t width, uint16_t *rgb, mode3_config_t *config, bool x_wrap)
{
    volatile const uint8_t *row_data = mode3_row_to_data(row, config, 4);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 4);
    int16_t col = -config->xpos_px;
    while (width)
    {
        if (col < 0)
        {
            if (x_wrap)
                col += (-(col + 1) / config->width_px + 1) * config->width_px;
            else
            {
                uint16_t empty_cols = -col;
                if (empty_cols > width)
                    empty_cols = width;
                memset(rgb, 0, sizeof(uint16_t) * empty_cols);
                col += empty_cols;
                rgb += empty_cols;
                width -= empty_cols;
                continue;
            }
        }
        if (col >= config->width_px)
        {
            if (x_wrap)
                col -= ((col - config->width_px) / config->width_px + 1) * config->width_px;
            else
            {
                memset(rgb, 0, sizeof(uint16_t) * width);
                break;
            }
        }
        int16_t fill_cols = width;
        if (fill_cols > config->width_px - col)
            fill_cols = config->width_px - col;
        width -= fill_cols;
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
mode3_render_4bpp_00xy_1r(int16_t scanline_id, int16_t width, uint16_t *rgb, void *config)
{
    int16_t row = mode3_scanline_to_row(scanline_id, config, false);
    return mode3_render_4bpp_1r(row, width, rgb, config, false);
}

static bool __attribute__((optimize("O1")))
mode3_render_4bpp_10xy_1r(int16_t scanline_id, int16_t width, uint16_t *rgb, void *config)
{
    int16_t row = mode3_scanline_to_row(scanline_id, config, false);
    return mode3_render_4bpp_1r(row, width, rgb, config, true);
}

static bool __attribute__((optimize("O1")))
mode3_render_4bpp_01xy_1r(int16_t scanline_id, int16_t width, uint16_t *rgb, void *config)
{
    int16_t row = mode3_scanline_to_row(scanline_id, config, true);
    return mode3_render_4bpp_1r(row, width, rgb, config, false);
}

static bool __attribute__((optimize("O1")))
mode3_render_4bpp_11xy_1r(int16_t scanline_id, int16_t width, uint16_t *rgb, void *config)
{
    int16_t row = mode3_scanline_to_row(scanline_id, config, true);
    return mode3_render_4bpp_1r(row, width, rgb, config, true);
}

static bool __attribute__((optimize("O1")))
mode3_render_8bpp(int16_t row, int16_t width, uint16_t *rgb, mode3_config_t *config, bool x_wrap)
{
    volatile const uint8_t *row_data = mode3_row_to_data(row, config, 8);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 8);
    int16_t col = -config->xpos_px;
    while (width)
    {
        if (col < 0)
        {
            if (x_wrap)
                col += (-(col + 1) / config->width_px + 1) * config->width_px;
            else
            {
                uint16_t empty_cols = -col;
                if (empty_cols > width)
                    empty_cols = width;
                memset(rgb, 0, sizeof(uint16_t) * empty_cols);
                col += empty_cols;
                rgb += empty_cols;
                width -= empty_cols;
                continue;
            }
        }
        if (col >= config->width_px)
        {
            if (x_wrap)
                col -= ((col - config->width_px) / config->width_px + 1) * config->width_px;
            else
            {
                memset(rgb, 0, sizeof(uint16_t) * width);
                break;
            }
        }
        int16_t fill_cols = width;
        if (fill_cols > config->width_px - col)
            fill_cols = config->width_px - col;
        width -= fill_cols;
        volatile const uint8_t *data = &row_data[col];
        col += fill_cols;
        for (; fill_cols; fill_cols--)
            *rgb++ = palette[*data];
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode3_render_8bpp_00xy(int16_t scanline_id, int16_t width, uint16_t *rgb, void *config)
{
    int16_t row = mode3_scanline_to_row(scanline_id, config, false);
    return mode3_render_8bpp(row, width, rgb, config, false);
}

static bool __attribute__((optimize("O1")))
mode3_render_8bpp_10xy(int16_t scanline_id, int16_t width, uint16_t *rgb, void *config)
{
    int16_t row = mode3_scanline_to_row(scanline_id, config, false);
    return mode3_render_8bpp(row, width, rgb, config, true);
}

static bool __attribute__((optimize("O1")))
mode3_render_8bpp_01xy(int16_t scanline_id, int16_t width, uint16_t *rgb, void *config)
{
    int16_t row = mode3_scanline_to_row(scanline_id, config, true);
    return mode3_render_8bpp(row, width, rgb, config, false);
}

static bool __attribute__((optimize("O1")))
mode3_render_8bpp_11xy(int16_t scanline_id, int16_t width, uint16_t *rgb, void *config)
{
    int16_t row = mode3_scanline_to_row(scanline_id, config, true);
    return mode3_render_8bpp(row, width, rgb, config, true);
}

static void *lookup_render_fn(uint16_t attributes)
{
    switch (attributes)
    {
    case 1:
        return mode3_render_4bpp_00xy_0r;
    case 2:
        return mode3_render_8bpp_00xy;
    case 5:
        return mode3_render_4bpp_10xy_0r;
    case 6:
        return mode3_render_8bpp_10xy;
    case 9:
        return mode3_render_4bpp_01xy_0r;
    case 10:
        return mode3_render_8bpp_01xy;
    case 13:
        return mode3_render_4bpp_11xy_0r;
    case 14:
        return mode3_render_8bpp_11xy;
    case 17:
        return mode3_render_4bpp_00xy_1r;
    case 18:
        return mode3_render_8bpp_00xy;
    case 21:
        return mode3_render_4bpp_10xy_1r;
    case 22:
        return mode3_render_8bpp_10xy;
    case 25:
        return mode3_render_4bpp_01xy_1r;
    case 26:
        return mode3_render_8bpp_01xy;
    case 29:
        return mode3_render_4bpp_11xy_1r;
    case 30:
        return mode3_render_8bpp_11xy;
    default:
        return NULL;
    }
}

bool mode3_prog(uint16_t *xregs)
{
    uint16_t attributes = xregs[3];
    uint16_t plane = xregs[4];
    int16_t scanline_begin = xregs[5];
    int16_t scanline_end = xregs[6];
    if (!scanline_end)
        scanline_end = vga_height();
    int16_t scanline_count = scanline_end - scanline_begin;

    // Validate
    if (xregs[2] > 0x10000 - sizeof(mode3_config_t) ||
        attributes >= 32 ||
        plane >= PICO_SCANVIDEO_PLANE_COUNT ||
        scanline_begin < 0 ||
        scanline_end > vga_height() ||
        scanline_count < 1)
        return false;

    void *render_fn = lookup_render_fn(attributes);
    if (!render_fn) // TODO remove after complete
        return false;
    mode3_config_t *config = (void *)&xram[xregs[2]];
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        vga_prog[i].fill_config[plane] = config;
        vga_prog[i].fill[plane] = render_fn;
    }
    return true;
}
