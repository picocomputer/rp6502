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

/* Each renderer copies the config out of XRAM once per scanline, checks the
 * copy and draws from it. A write to XRAM part way through a line, such as one
 * byte of a 16-bit size, then cannot slip in behind the checks. */

static uint16_t mode2_options[VGA_PROG_MAX][SCANVIDEO_PLANE_COUNT];

// tile_w and tile_h are the tile's on-screen size: tile_size, less any X or Y
// trim. A trimmed height need not be a power of two, so the row within the
// tile needs a modulo rather than a mask.
static const uint8_t *
mode2_scanline_to_data(int16_t scanline_id, int16_t width, mode2_config_t *config,
                       int16_t tile_w, int16_t tile_h, int16_t *row)
{
    // The size in pixels has to fit an int16_t, and be at least 1, before the
    // wrap divides by it. The counts alone would let 4096 tiles overflow.
    if (config->width_tiles < 1 ||
        (int32_t)config->width_tiles * tile_w > INT16_MAX ||
        config->height_tiles < 1 ||
        (int32_t)config->height_tiles * tile_h > INT16_MAX)
        return NULL;
    *row = scanline_id - config->y_pos_px;
    const int16_t height = config->height_tiles * tile_h;
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
    // draws nothing. mode2_fill_cols counts on it: unwrapped, a column past
    // the right of the map would leave it a negative run.
    if (!config->x_wrap &&
        (config->x_pos_px >= width || config->x_pos_px + config->width_tiles * tile_w <= 0))
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

// One pixel of a tile row, for the columns of a tile cut by the edge of the
// map or the canvas. Leftmost pixel in the most significant bits.
static inline __attribute__((always_inline)) uint16_t
mode2_pixel(const uint8_t *src, unsigned x, const uint16_t *pal, int16_t bpp)
{
    if (bpp == 8)
        return pal[src[x]];
    const unsigned ppb = 8 / bpp;
    const unsigned shift = 8 - bpp * (x % ppb + 1);
    return pal[(src[x / ppb] >> shift) & ((1u << bpp) - 1)];
}

// Columns x to x + n - 1 of one tile row. A tile past the end of XRAM draws
// transparent, as mode 2 does not require a full tile set.
static inline __attribute__((always_inline)) void
mode2_emit_part(uint16_t *rgb, uint8_t id, uint32_t ids, uint32_t base, uint32_t tile_bytes,
                unsigned x, unsigned n, const uint16_t *pal, int16_t bpp)
{
    if (id >= ids)
    {
        memset(rgb, 0, sizeof(uint16_t) * n);
        return;
    }
    const uint8_t *src = (const uint8_t *)&xram[base + id * tile_bytes];
    for (unsigned i = 0; i < n; i++)
        rgb[i] = mode2_pixel(src, x + i, pal, bpp);
}

// Whole tiles, the hot loop, a word of two pixels to a store. Whether the run
// starts word aligned makes no measurable difference, so one copy serves. 1bpp
// expands a nibble at a time through a table of the 16 ways four pixels can
// land, 2bpp a nibble at a time through the 16 pixel pairs, and 4bpp and 8bpp
// read the row a word at a time. tiles is the scanline's row of tile 0, and a
// tile ID of ids or more is past the end of XRAM.
static inline __attribute__((always_inline)) void
mode2_emit_tiles(uint16_t *rgb, const uint8_t *map, unsigned count,
                 uint32_t ids, const uint8_t *tiles, uint32_t tile_bytes,
                 const uint32_t *tbl, const uint16_t *pal,
                 int16_t tile_size, int16_t bpp)
{
    const unsigned row_bytes = (unsigned)tile_size * bpp / 8;
    const unsigned words = (unsigned)tile_size / 2;
    if (!count)
        return;
    do
    {
        const uint32_t id = *map++;
        if (__builtin_expect(id >= ids, 0))
        {
            memset(rgb, 0, sizeof(uint16_t) * tile_size);
            rgb += tile_size;
            continue;
        }
        const uint8_t *src = tiles + id * tile_bytes;
        uint32_t w[16];
        if (bpp == 8)
            // A word of the row at a time: one load for four pixels.
            for (unsigned k = 0; k < row_bytes; k += 4)
            {
                const uint32_t q = mode_load32(src + k);
                w[k / 2 + 0] = mode_pack2(pal[q & 0xFF], pal[(q >> 8) & 0xFF]);
                w[k / 2 + 1] = mode_pack2(pal[(q >> 16) & 0xFF], pal[q >> 24]);
            }
        else if (bpp == 4)
            for (unsigned k = 0; k < row_bytes; k += 4)
            {
                const uint32_t q = mode_load32(src + k);
                w[k + 0] = mode_pack2(pal[(q >> 4) & 0xF], pal[q & 0xF]);
                w[k + 1] = mode_pack2(pal[(q >> 12) & 0xF], pal[(q >> 8) & 0xF]);
                w[k + 2] = mode_pack2(pal[(q >> 20) & 0xF], pal[(q >> 16) & 0xF]);
                w[k + 3] = mode_pack2(pal[q >> 28], pal[(q >> 24) & 0xF]);
            }
        else
            for (unsigned k = 0; k < row_bytes; k++)
            {
                const uint32_t b = src[k];
                if (bpp == 1)
                {
                    const uint32_t *hi = &tbl[(b >> 4) * 2];
                    const uint32_t *lo = &tbl[(b & 0xF) * 2];
                    w[k * 4 + 0] = hi[0];
                    w[k * 4 + 1] = hi[1];
                    w[k * 4 + 2] = lo[0];
                    w[k * 4 + 3] = lo[1];
                }
                else
                {
                    w[k * 2 + 0] = tbl[b >> 4];
                    w[k * 2 + 1] = tbl[b & 0xF];
                }
            }
        for (unsigned k = 0; k < words; k++)
            ((mode_word_t *)rgb)[k] = w[k];
        rgb += tile_size;
    } while (--count);
}

// On-screen tiles align with the data bytes, so the map is walked a tile at a
// time and each tile row a byte at a time, with no addressing per pixel. Only
// the tiles cut by an edge are drawn a pixel at a time. bpp and tile_size are
// constants at every call, so each instantiation folds to one bit depth.
static inline __attribute__((always_inline)) void
mode2_emit_full(mode2_config_t *config, uint16_t *rgb, int16_t width,
                const uint8_t *row_data, int16_t row,
                const uint16_t *pal, int16_t tile_size, int16_t bpp)
{
    const uint32_t row_bytes = (uint32_t)tile_size * bpp / 8;
    const uint32_t tile_bytes = row_bytes * tile_size;
    const uint32_t base = (uint32_t)config->xram_tile_ptr + row_bytes * row;
    // The tile IDs whose row this scanline lies wholly in XRAM.
    uint32_t ids = 0;
    if (base + row_bytes <= 0x10000)
        ids = (0x10000 - row_bytes - base) / tile_bytes + 1;
    const uint8_t *tiles = (const uint8_t *)&xram[base & 0xFFFF];
    MODE_TABLE(tbl, 32);
    if (bpp == 1)
        mode_nibble_set(tbl, pal[0], pal[1]);
    else if (bpp == 2)
        mode_quad_set(tbl, pal);
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width, tile_size);
        if (fill_cols <= 0)
            continue;
        const uint8_t *map = row_data + col / tile_size;
        const unsigned x = col & (tile_size - 1);
        col += fill_cols;
        if (x)
        {
            unsigned n = tile_size - x;
            if (n > (unsigned)fill_cols)
                n = fill_cols;
            mode2_emit_part(rgb, *map++, ids, base, tile_bytes, x, n, pal, bpp);
            rgb += n;
            fill_cols -= n;
        }
        const unsigned count = fill_cols / tile_size;
        mode2_emit_tiles(rgb, map, count, ids, tiles, tile_bytes, tbl, pal, tile_size, bpp);
        rgb += count * tile_size;
        map += count;
        fill_cols &= tile_size - 1;
        if (fill_cols)
        {
            mode2_emit_part(rgb, *map, ids, base, tile_bytes, 0, fill_cols, pal, bpp);
            rgb += fill_cols;
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
    mode2_config_t cfg = *(const mode2_config_t *)&xram[config_ptr];
    mode2_config_t *config = &cfg;
    int16_t row;
    const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, width, config, tile_size - x_trim,
                               tile_size - y_trim, &row);
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
    mode2_config_t cfg = *(const mode2_config_t *)&xram[config_ptr];
    mode2_config_t *config = &cfg;
    int16_t row;
    const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, width, config, tile_size - x_trim,
                               tile_size - y_trim, &row);
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
    mode2_config_t cfg = *(const mode2_config_t *)&xram[config_ptr];
    mode2_config_t *config = &cfg;
    int16_t row;
    const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, width, config, tile_size - x_trim,
                               tile_size - y_trim, &row);
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
    mode2_config_t cfg = *(const mode2_config_t *)&xram[config_ptr];
    mode2_config_t *config = &cfg;
    int16_t row;
    const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, width, config, tile_size - x_trim,
                               tile_size - y_trim, &row);
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
