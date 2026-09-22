/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/vga/mode/mode2.h"
#include "core/vga/mode/mode.h"
#include "core/vga/vga.h"
#include "core/sys/xram.h"
#include "core/vga/pixel_format.h"
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
    int16_t width_tiles;
    int16_t height_tiles;
    uint16_t xram_data_ptr;
    uint16_t xram_palette_ptr;
    uint16_t xram_tile_ptr;
} mode2_config_t;

static uint16_t mode2_options[VGA_PROG_MAX][SCANVIDEO_PLANE_COUNT];

// tile_h is the tile's on-screen height: tile_size, less any Y trim. A trimmed
// height need not be a power of two, so the row within the tile needs a modulo
// rather than a mask.
static const uint8_t *
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
    const uint8_t *rv = (const uint8_t *)&xram[config->xram_data_ptr + *row / tile_h * sizeof_row];
    *row %= tile_h;
    return rv;
}

static const uint16_t *
mode2_get_palette(mode2_config_t *config, int16_t bpp)
{
    if (!(config->xram_palette_ptr & 1) &&
        config->xram_palette_ptr <= 0x10000 - sizeof(uint16_t) * (1 << bpp))
        return (const uint16_t *)&xram[config->xram_palette_ptr];
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
                        int16_t col, int16_t row, const uint8_t *row_data, uint16_t *index)
{
    uint32_t row_size = tile_size * bpp / 8;
    uint32_t mem_size = row_size * tile_size;
    uint8_t tile_id = row_data[col / tile_size];
    uint8_t pixels_per_byte = 8 / bpp;
    *index = (col / pixels_per_byte) & (tile_size / pixels_per_byte - 1);
    return (uint32_t)config->xram_tile_ptr + mem_size * tile_id + row_size * row;
}

// Mode 2 does not require a full tile set in XRAM.
static inline __attribute__((always_inline)) void
mode2_skip_tile(uint16_t **rgb, int16_t *col, int16_t fill_cols,
                int16_t *width, int16_t tile_size)
{
    int16_t skip = tile_size - (*col & (tile_size - 1));
    if (skip > fill_cols)
        skip = fill_cols;
    memset(*rgb, 0, sizeof(uint16_t) * skip);
    *rgb += skip;
    *col += skip;
    *width += fill_cols - skip;
}

// On-screen tiles align with the data bytes, so one read of xram yields a whole
// run of pixels: 8 at 1bpp, 4 at 2bpp, and so on. bpp and tile_size are
// constants at every call, so each instantiation folds to one bit depth's byte
// walk.
static inline __attribute__((always_inline)) void
mode2_emit_full(mode2_config_t *config, uint16_t *rgb, int16_t width,
                const uint8_t *row_data, int16_t row,
                const uint16_t *pal, int16_t tile_size, int16_t bpp)
{
    const uint32_t tile_hi = 0x10000 - (uint32_t)tile_size * bpp / 8;
    int16_t col = -config->x_pos_px;
    if (bpp == 1)
    {
        MODE_TABLE(pair, 4);
        mode_pair_set(pair, pal[0], pal[1]);
        while (width)
        {
            int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width, tile_size);
            uint16_t index;
            uint32_t tile_mem = mode2_get_tile_row_addr(config, 1, tile_size, col, row, row_data, &index);
            if (tile_mem > tile_hi)
                goto skip_1;
            uint8_t bits = xram[tile_mem + index];
            int16_t start = col & 7;
            int16_t part = 8 - start;
            if (part > fill_cols)
                part = fill_cols;
            fill_cols -= part;
            col += part;
            mode_emit_head_1bpp(&rgb, bits, pal, start, part);
            if (++index == tile_size / 8)
            {
                tile_mem = mode2_get_tile_row_addr(config, 1, tile_size, col, row, row_data, &index);
                if (tile_mem > tile_hi)
                    goto skip_1;
            }
            bits = xram[tile_mem + index];
            while (fill_cols > 7)
            {
                mode_render_1bpp(rgb, bits, pair);
                rgb += 8;
                fill_cols -= 8;
                col += 8;
                if (++index == tile_size / 8)
                {
                    tile_mem = mode2_get_tile_row_addr(config, 1, tile_size, col, row, row_data, &index);
                    if (tile_mem > tile_hi)
                        goto skip_1;
                }
                bits = xram[tile_mem + index];
            }
            col += fill_cols;
            mode_emit_tail_1bpp(&rgb, bits, pal, fill_cols);
            continue;
        skip_1:
            mode2_skip_tile(&rgb, &col, fill_cols, &width, tile_size);
        }
    }
    else if (bpp == 2)
    {
        MODE_TABLE(quad, 16);
        mode_quad_set(quad, pal);
        while (width)
        {
            int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width, tile_size);
            uint16_t index;
            uint32_t tile_mem = mode2_get_tile_row_addr(config, 2, tile_size, col, row, row_data, &index);
            if (tile_mem > tile_hi)
                goto skip_2;
            uint8_t bits = xram[tile_mem + index];
            int16_t start = col & 3;
            int16_t part = 4 - start;
            if (part > fill_cols)
                part = fill_cols;
            fill_cols -= part;
            col += part;
            mode_emit_head_2bpp(&rgb, bits, pal, start, part);
            if (++index == tile_size / 4)
            {
                tile_mem = mode2_get_tile_row_addr(config, 2, tile_size, col, row, row_data, &index);
                if (tile_mem > tile_hi)
                    goto skip_2;
            }
            bits = xram[tile_mem + index];
            while (fill_cols > 3)
            {
                mode_render_2bpp(rgb, bits, quad);
                rgb += 4;
                fill_cols -= 4;
                col += 4;
                if (++index == tile_size / 4)
                {
                    tile_mem = mode2_get_tile_row_addr(config, 2, tile_size, col, row, row_data, &index);
                    if (tile_mem > tile_hi)
                        goto skip_2;
                }
                bits = xram[tile_mem + index];
            }
            col += fill_cols;
            mode_emit_tail_2bpp(&rgb, bits, pal, fill_cols);
            continue;
        skip_2:
            mode2_skip_tile(&rgb, &col, fill_cols, &width, tile_size);
        }
    }
    else if (bpp == 4)
    {
        while (width)
        {
            int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width, tile_size);
            while (fill_cols > 0)
            {
                // One addressing pass per tile row, as at 8bpp. The pair written
                // per store is the byte's own two nibbles, so an odd column has
                // to be spent before the run can be walked a byte at a time.
                uint16_t index;
                uint32_t tile_mem = mode2_get_tile_row_addr(config, 4, tile_size, col, row, row_data, &index);
                if (tile_mem > tile_hi)
                    goto skip_4;
                const bool odd = (col & 1) != 0;
                int16_t run = (int16_t)((tile_size / 2 - (int16_t)index) * 2 - (odd ? 1 : 0));
                if (run > fill_cols)
                    run = fill_cols;
                const uint8_t *src = (const uint8_t *)&xram[tile_mem + index];
                fill_cols -= run;
                col += run;
                if (odd)
                {
                    *rgb++ = pal[*src++ & 0xF];
                    run--;
                }
                const uint8_t *const end = src + (run >> 1);
                for (; src < end; rgb += 2, src++)
                    *(mode_word_t *)rgb = mode_pack2(pal[*src >> 4], pal[*src & 0xF]);
                if (run & 1)
                    *rgb++ = pal[*src >> 4];
            }
            continue;
        skip_4:
            mode2_skip_tile(&rgb, &col, fill_cols, &width, tile_size);
        }
    }
    else
    {
        while (width)
        {
            int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width, tile_size);
            while (fill_cols > 0)
            {
                // A tile's row is contiguous, so address it once and take the
                // whole run rather than retesting the boundary every pixel.
                uint16_t index;
                uint32_t tile_mem = mode2_get_tile_row_addr(config, 8, tile_size, col, row, row_data, &index);
                if (tile_mem > tile_hi)
                    goto skip_8;
                int16_t run = tile_size - (int16_t)index;
                if (run > fill_cols)
                    run = fill_cols;
                const uint8_t *src = (const uint8_t *)&xram[tile_mem + index];
                fill_cols -= run;
                col += run;
                if ((uintptr_t)rgb & 2)
                {
                    *rgb++ = pal[*src++];
                    run--;
                }
                const uint8_t *const end = src + (run & ~1);
                for (; src < end; rgb += 2, src += 2)
                    *(mode_word_t *)rgb = mode_pack2(pal[src[0]], pal[src[1]]);
                if (run & 1)
                    *rgb++ = pal[*src];
            }
            continue;
        skip_8:
            mode2_skip_tile(&rgb, &col, fill_cols, &width, tile_size);
        }
    }
}

// With an X trim the effective tile is eff_w pixels wide, so on-screen tiles no
// longer align with the data bytes and the byte walk above cannot be used. The
// tile data is still stored at the full tile_size. A Y trim alone leaves eff_w
// == tile_size and still comes here.
static inline __attribute__((always_inline)) void
mode2_emit_trim(mode2_config_t *config, uint16_t *rgb, int16_t width,
                const uint8_t *row_data, int16_t row, int16_t eff_w,
                const uint16_t *pal, int16_t tile_size, int16_t bpp)
{
    const uint32_t row_size = (uint32_t)tile_size * bpp / 8;
    const uint32_t mem_size = row_size * tile_size;
    const uint32_t row_off = (uint32_t)config->xram_tile_ptr + row_size * row;
    const uint32_t tile_hi = 0x10000 - row_size;
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
            if (tile_mem > tile_hi)
            {
                memset(rgb, 0, sizeof(uint16_t) * run);
                rgb += run;
                continue;
            }
            // A trimmed tile does not align with the data bytes, so the byte
            // walk cannot cross tiles, but within one run it still holds: fetch
            // a byte and shift it along rather than re-address every pixel.
            if (bpp == 8)
            {
                const uint8_t *src = (const uint8_t *)&xram[tile_mem] + col_in_tile;
                int16_t n = run;
                if ((uintptr_t)rgb & 2)
                {
                    *rgb++ = pal[*src++];
                    n--;
                }
                const uint8_t *const end = src + (n & ~1);
                for (; src < end; rgb += 2, src += 2)
                    *(mode_word_t *)rgb = mode_pack2(pal[src[0]], pal[src[1]]);
                if (n & 1)
                    *rgb++ = pal[*src];
            }
            else
            {
                const unsigned ppb = 8 / bpp;
                const unsigned phase = (unsigned)col_in_tile % ppb;
                const uint8_t *src = (const uint8_t *)&xram[tile_mem] + (unsigned)col_in_tile / ppb;
                unsigned left = ppb - phase;
                uint8_t bits = (uint8_t)(*src++ << (phase * bpp));
                for (int16_t i = 0; i < run; i++)
                {
                    if (left == 0)
                    {
                        bits = *src++;
                        left = ppb;
                    }
                    *rgb++ = pal[bits >> (8 - bpp)];
                    bits = (uint8_t)(bits << bpp);
                    left--;
                }
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
    const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, tile_size - y_trim, &row);
    if (!row_data)
        return false;
    const uint16_t *palette = mode2_get_palette(config, 1);
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
    const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, tile_size - y_trim, &row);
    if (!row_data)
        return false;
    const uint16_t *palette = mode2_get_palette(config, 2);
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
    const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, tile_size - y_trim, &row);
    if (!row_data)
        return false;
    const uint16_t *palette = mode2_get_palette(config, 4);
    if (x_trim || y_trim)
        mode2_emit_trim(config, rgb, width, row_data, row, tile_size - x_trim, palette, tile_size, 4);
    else
        mode2_emit_full(config, rgb, width, row_data, row, palette, tile_size, 4);
    return true;
}

static inline __attribute__((always_inline)) bool
mode2_render_8bpp(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr,
                  int16_t x_trim, int16_t y_trim, int16_t tile_size)
{
    mode2_config_t *config = (void *)&xram[config_ptr];
    int16_t row;
    const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, config, tile_size - y_trim, &row);
    if (!row_data)
        return false;
    const uint16_t *palette = mode2_get_palette(config, 8);
    if (x_trim || y_trim)
        mode2_emit_trim(config, rgb, width, row_data, row, tile_size - x_trim, palette, tile_size, 8);
    else
        mode2_emit_full(config, rgb, width, row_data, row, palette, tile_size, 8);
    return true;
}

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

vga_fill_fn_t mode2_fill_fn(uint16_t attributes)
{
    return (attributes & 0xF000) ? NULL : mode2_render;
}

bool mode2_fill_attr(int16_t scanline, int16_t plane, uint16_t *attributes)
{
    if (scanline < 0 || scanline >= VGA_PROG_MAX ||
        plane < 0 || plane >= SCANVIDEO_PLANE_COUNT)
        return false;
    *attributes = mode2_options[scanline][plane];
    return true;
}

void mode2_set_options(int16_t scanline, int16_t plane, uint16_t options)
{
    if (scanline >= 0 && scanline < VGA_PROG_MAX &&
        plane >= 0 && plane < SCANVIDEO_PLANE_COUNT)
        mode2_options[scanline][plane] = options;
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
