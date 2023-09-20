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

static bool __attribute__((optimize("O1")))
mode3_render_4bpp_1r(int16_t row, int16_t width, uint16_t *rgb, mode3_config_t *config, bool x_wrap)
{
    if (row < 0)
        return false;

    const uint16_t *palette = color256;
    if (config->xram_palette_ptr <= 0x10000 - sizeof(uint16_t) * 16)
        palette = (void *)&xram[config->xram_palette_ptr];

    int32_t sizeof_row = ((int32_t)config->width_px * 4 + 7) / 8;
    int32_t sizeof_bitmap = (int32_t)config->height_px * sizeof_row;
    if (sizeof_row < 1 || sizeof_bitmap > 0x10000 - config->xram_data_ptr)
        return false;

    uint8_t *row_data = (void *)&xram[config->xram_data_ptr + row * sizeof_row];

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
            }
        }
        if (col >= config->width_px)
        {
            if (x_wrap)
                col -= ((col - config->width_px) / config->width_px + 1) * config->width_px;
            else
            {
                memset(rgb, 0, sizeof(uint16_t) * width);
                col += width;
                rgb += width;
                width = 0;
            }
        }
        uint16_t fill_cols = width;
        if (fill_cols > config->width_px - col)
            fill_cols = config->width_px - col;
        width -= fill_cols;
        uint8_t *data = &row_data[col / 2];
        if (col & 1)
        {
            *rgb++ = palette[*data++ >> 4];
            col++;
            fill_cols--;
        }
        for (; fill_cols > 1; fill_cols -= 2, data++)
        {
            *rgb++ = palette[*data & 0xF];
            *rgb++ = palette[*data >> 4];
        }
        if (fill_cols == 1)
        {
            *rgb++ = palette[*data++ & 0xF];
            col++;
        }
    }
    return true;
}

static bool __attribute__((optimize("O1")))
mode3_render_4bpp_00xy_1r(int16_t scanline_id, int16_t width, uint16_t *rgb, void *config)
{
    int16_t row = mode3_scanline_to_row(scanline_id, config, false);
    return mode3_render_4bpp_1r(row, width, rgb, config, false);
}

static void *lookup_render_fn(uint16_t attributes)
{
    switch (attributes)
    {
    case 17:
        return mode3_render_4bpp_00xy_1r;
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
        vga_prog[i].fill[plane] = render_fn;
        vga_prog[i].fill_config[plane] = config;
    }
    return true;
}
