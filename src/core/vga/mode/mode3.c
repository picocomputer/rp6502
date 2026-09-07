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

static volatile const uint8_t *
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

static volatile const uint16_t *
mode3_get_palette(mode3_config_t *config, int16_t bpp)
{
    if (!(config->xram_palette_ptr & 1) &&
        config->xram_palette_ptr <= 0x10000 - sizeof(uint16_t) * (1 << bpp))
        return (uint16_t *)&xram[config->xram_palette_ptr];
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

static bool
mode3_render_1bpp(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    (void)plane_id;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 1);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 1);
    uint16_t pal[2] = {palette[0], palette[1]};
    int16_t col = -config->x_pos_px;
    int16_t width_px = config->width_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col / 8];
        int16_t start = col & 7;
        int16_t part = 8 - start;
        if (part > width_px - col)
            part = width_px - col;
        if (part > fill_cols)
            part = fill_cols;
        fill_cols -= part;
        col += part;
        mode_emit_head_1bpp(&rgb, *data++, pal, start, part);
        col += fill_cols;
        while (fill_cols > 7)
        {
            mode_render_1bpp(rgb, *data++, pal[0], pal[1]);
            rgb += 8;
            fill_cols -= 8;
        }
        mode_emit_tail_1bpp(&rgb, *data, pal, fill_cols);
    }
    return true;
}

static bool
mode3_render_1bpp_reverse(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    (void)plane_id;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 1);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 1);
    uint16_t pal[2] = {palette[0], palette[1]};
    int16_t col = -config->x_pos_px;
    int16_t width_px = config->width_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col / 8];
        int16_t start = col & 7;
        int16_t part = 8 - start;
        if (part > width_px - col)
            part = width_px - col;
        if (part > fill_cols)
            part = fill_cols;
        fill_cols -= part;
        col += part;
        mode_emit_head_1bpp_reverse(&rgb, *data++, pal, start, part);
        col += fill_cols;
        while (fill_cols > 7)
        {
            mode_render_1bpp_reverse(rgb, *data++, pal[0], pal[1]);
            rgb += 8;
            fill_cols -= 8;
        }
        mode_emit_tail_1bpp_reverse(&rgb, *data, pal, fill_cols);
    }
    return true;
}

static bool
mode3_render_2bpp(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    (void)plane_id;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 2);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 2);
    uint16_t pal[4] = {palette[0], palette[1], palette[2], palette[3]};
    int16_t col = -config->x_pos_px;
    int16_t width_px = config->width_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col / 4];
        int16_t start = col & 3;
        int16_t part = 4 - start;
        if (part > width_px - col)
            part = width_px - col;
        if (part > fill_cols)
            part = fill_cols;
        fill_cols -= part;
        col += part;
        mode_emit_head_2bpp(&rgb, *data++, pal, start, part);
        col += fill_cols;
        while (fill_cols > 3)
        {
            *rgb++ = pal[(*data & 0xC0) >> 6];
            *rgb++ = pal[(*data & 0x30) >> 4];
            *rgb++ = pal[(*data & 0x0C) >> 2];
            *rgb++ = pal[*data++ & 0x03];
            fill_cols -= 4;
        }
        mode_emit_tail_2bpp(&rgb, *data, pal, fill_cols);
    }
    return true;
}

static bool
mode3_render_2bpp_reverse(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    (void)plane_id;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 2);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 2);
    uint16_t pal[4] = {palette[0], palette[1], palette[2], palette[3]};
    int16_t col = -config->x_pos_px;
    int16_t width_px = config->width_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col / 4];
        int16_t start = col & 3;
        int16_t part = 4 - start;
        if (part > width_px - col)
            part = width_px - col;
        if (part > fill_cols)
            part = fill_cols;
        fill_cols -= part;
        col += part;
        mode_emit_head_2bpp_reverse(&rgb, *data++, pal, start, part);
        col += fill_cols;
        while (fill_cols > 3)
        {
            *rgb++ = pal[*data & 0x03];
            *rgb++ = pal[(*data & 0x0C) >> 2];
            *rgb++ = pal[(*data & 0x30) >> 4];
            *rgb++ = pal[(*data++ & 0xC0) >> 6];
            fill_cols -= 4;
        }
        mode_emit_tail_2bpp_reverse(&rgb, *data, pal, fill_cols);
    }
    return true;
}

static bool
mode3_render_4bpp(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    (void)plane_id;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 4);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 4);
    uint16_t pal[16];
    for (int i = 0; i < 16; i++)
        pal[i] = palette[i];
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col / 2];
        if (col & 1)
        {
            *rgb++ = pal[*data++ & 0xF];
            col++;
            fill_cols--;
        }
        col += fill_cols;
        while (fill_cols > 1)
        {
            *rgb++ = pal[*data >> 4];
            *rgb++ = pal[*data++ & 0xF];
            fill_cols -= 2;
        }
        if (fill_cols == 1)
            *rgb++ = pal[*data >> 4];
    }
    return true;
}

static bool
mode3_render_4bpp_reverse(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    (void)plane_id;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 4);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 4);
    uint16_t pal[16];
    for (int i = 0; i < 16; i++)
        pal[i] = palette[i];
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col / 2];
        if (col & 1)
        {
            *rgb++ = pal[*data++ >> 4];
            col++;
            fill_cols--;
        }
        col += fill_cols;
        while (fill_cols > 1)
        {
            *rgb++ = pal[*data & 0xF];
            *rgb++ = pal[*data++ >> 4];
            fill_cols -= 2;
        }
        if (fill_cols == 1)
            *rgb++ = pal[*data & 0xF];
    }
    return true;
}

static bool
mode3_render_8bpp(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    (void)plane_id;
    mode3_config_t *config = (void *)&xram[config_ptr];
    volatile const uint8_t *row_data = mode3_scanline_to_data(scanline_id, config, 8);
    if (!row_data)
        return false;
    volatile const uint16_t *palette = mode3_get_palette(config, 8);
    uint16_t pal[256];
    for (int i = 0; i < 256; i++)
        pal[i] = palette[i];
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode3_fill_cols(config, &rgb, &col, &width);
        volatile const uint8_t *data = &row_data[col];
        col += fill_cols;
        for (; fill_cols; fill_cols--)
            *rgb++ = pal[*data++];
    }
    return true;
}

static bool
mode3_render_16bpp(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    (void)plane_id;
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

/* The renderer an attribute names, and the attribute a renderer came from.
 * A savestate carries the attribute: a function's address is this build's own
 * and means nothing to the build that loads the blob.
 *
 * The reverse walks the forward rather than keeping a second table, so the
 * two cannot drift apart when a renderer is added. */
vga_fill_fn_t mode3_fill_fn(uint16_t attributes)
{
    switch (attributes)
    {
    case 0:
        return mode3_render_1bpp;
    case 1:
        return mode3_render_2bpp;
    case 2:
        return mode3_render_4bpp;
    case 3:
        return mode3_render_8bpp;
    case 4:
        return mode3_render_16bpp;
    case 8:
        return mode3_render_1bpp_reverse;
    case 9:
        return mode3_render_2bpp_reverse;
    case 10:
        return mode3_render_4bpp_reverse;
    default:
        return NULL;
    }
}

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

    vga_fill_fn_t render_fn = mode3_fill_fn(attributes);
    if (!render_fn)
        return false;

    return vga_prog_fill(plane, scanline_begin, scanline_end, config_ptr, render_fn);
}

#pragma GCC pop_options
