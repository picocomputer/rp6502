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

#define MODE2_CUSTOM 0x1000
#define MODE2_HDOUBLE 0x2000
#define MODE2_VDOUBLE 0x4000

// The shape of a tile, from the OPTIONS register. A tile's own pixels are
// tex_w by tex_h, stored row_bytes to a row and tile_bytes to a tile. On the
// canvas, doubling makes it scr_w by scr_h.
typedef struct
{
    int16_t tex_w, tex_h;
    int16_t scr_w, scr_h;
    uint32_t row_bytes, tile_bytes;
    bool hdouble, vdouble;
} mode2_geom_t;

static inline __attribute__((always_inline)) void
mode2_geometry(uint16_t opt, int16_t bpp, mode2_geom_t *g)
{
    if (opt & MODE2_CUSTOM)
    {
        // A custom tile is 4 to 64 pixels each way in steps of 4, its rows
        // packed, each starting on a new byte.
        g->tex_w = (int16_t)((((opt >> 4) & 0x0F) + 1) * 4);
        g->tex_h = (int16_t)((((opt >> 8) & 0x0F) + 1) * 4);
        g->row_bytes = ((uint32_t)g->tex_w * bpp + 7) / 8;
        g->tile_bytes = g->row_bytes * g->tex_h;
    }
    else
    {
        // A trimmed tile keeps the full tile's layout and drops columns and
        // rows from what is shown.
        const int16_t size = (opt & 0x08) ? 16 : 8;
        g->tex_w = (int16_t)(size - ((opt >> 4) & 0x0F));
        g->tex_h = (int16_t)(size - ((opt >> 8) & 0x0F));
        g->row_bytes = (uint32_t)size * bpp / 8;
        g->tile_bytes = g->row_bytes * size;
    }
    g->hdouble = (opt & MODE2_HDOUBLE) != 0;
    g->vdouble = (opt & MODE2_VDOUBLE) != 0;
    g->scr_w = (int16_t)(g->tex_w << g->hdouble);
    g->scr_h = (int16_t)(g->tex_h << g->vdouble);
}

// One pixel of a tile row, leftmost pixel in the most significant bits.
static inline __attribute__((always_inline)) uint16_t
mode2_pixel(const uint8_t *src, unsigned x, const uint16_t *pal, int16_t bpp)
{
    if (bpp == 8)
        return pal[src[x]];
    const unsigned ppb = 8 / bpp;
    const unsigned shift = 8 - bpp * (x % ppb + 1);
    return pal[(src[x / ppb] >> shift) & ((1u << bpp) - 1)];
}

// Canvas columns x to x + n - 1 of one tile on this scanline, through any
// doubling. A tile past the end of XRAM draws transparent, as mode 2 does not
// require a full tile set. The source byte is walked in a register rather
// than addressed for every pixel.
static inline __attribute__((always_inline)) void
mode2_emit_part(uint16_t *rgb, uint8_t id, uint32_t ids, const uint8_t *tiles,
                uint32_t tile_bytes, unsigned x, unsigned n, bool hdouble,
                const uint16_t *pal, int16_t bpp)
{
    if (id >= ids)
    {
        memset(rgb, 0, sizeof(uint16_t) * n);
        return;
    }
    const uint8_t *row = tiles + id * tile_bytes;
    unsigned tx = x >> hdouble;
    if (hdouble && (x & 1))
    {
        // The right half of a doubled pixel.
        *rgb++ = mode2_pixel(row, tx++, pal, bpp);
        n--;
    }
    if (bpp == 8)
    {
        const uint8_t *src = row + tx;
        if (hdouble)
        {
            for (; n >= 2; n -= 2, rgb += 2)
                *(mode_word_t *)rgb = pal[*src++] * 0x10001u;
            if (n)
                *rgb = pal[*src];
        }
        else
            for (; n; n--)
                *rgb++ = pal[*src++];
        return;
    }
    const unsigned ppb = 8 / bpp;
    const uint8_t *src = row + tx / ppb;
    unsigned left = ppb - tx % ppb;
    uint8_t bits = (uint8_t)(*src++ << (tx % ppb * bpp));
    while (n)
    {
        if (!left)
        {
            bits = *src++;
            left = ppb;
        }
        const uint16_t color = pal[bits >> (8 - bpp)];
        bits = (uint8_t)(bits << bpp);
        left--;
        if (hdouble && n >= 2)
        {
            *(mode_word_t *)rgb = color * 0x10001u;
            rgb += 2;
            n -= 2;
        }
        else
        {
            *rgb++ = color;
            n--;
        }
    }
}

// Whole tiles, the hot loop. Every tile decodes its whole stored row, a byte
// at a time, and the run steps on by the tile's width on the canvas. Pixels
// past that width, the trimmed columns and the padding of a row that ends
// part way into a byte, are covered by the next tile, so the caller only
// sends tiles whose whole row fits in the line. Stores are a word of two
// pixels each; whether the run starts word aligned makes no measurable
// difference, so one copy serves.
//
// Undoubled, 1bpp expands a nibble at a time through a table of the 16 ways
// four pixels can land, 2bpp a nibble at a time through the 16 pixel pairs,
// and 4bpp a byte at a time through the plane's mode_4bpp_pairs; 4bpp and
// 8bpp read the row a word at a time. Doubled, every pixel is a
// word, through a table of the doubled colors at 1bpp and 2bpp. tiles is the
// scanline's row of tile 0, and a tile ID of ids or more is past the end of
// XRAM.
static inline __attribute__((always_inline)) void
mode2_emit_tiles(uint16_t *rgb, const uint8_t *map, unsigned count,
                 uint32_t ids, const uint8_t *tiles, uint32_t tile_bytes,
                 uint32_t row_bytes, int16_t step,
                 const uint32_t *tbl, const uint16_t *pal, int16_t bpp, bool hdouble)
{
    for (; count; count--, rgb += step)
    {
        const uint32_t id = *map++;
        if (id >= ids)
        {
            memset(rgb, 0, sizeof(uint16_t) * step);
            continue;
        }
        const uint8_t *src = tiles + id * tile_bytes;
        mode_word_t *w = (mode_word_t *)rgb;
        unsigned k = 0;
        if (hdouble)
#pragma GCC unroll 64
            for (; k < row_bytes; k++)
            {
                const uint32_t b = src[k];
                if (bpp == 8)
                    *w++ = pal[b] * 0x10001u;
                else if (bpp == 4)
                {
                    const uint32_t c0 = pal[b >> 4] * 0x10001u;
                    const uint32_t c1 = pal[b & 0xF] * 0x10001u;
                    w[0] = c0;
                    w[1] = c1;
                    w += 2;
                }
                else if (bpp == 2)
                {
                    const uint32_t c0 = tbl[b >> 6], c1 = tbl[(b >> 4) & 3];
                    const uint32_t c2 = tbl[(b >> 2) & 3], c3 = tbl[b & 3];
                    w[0] = c0;
                    w[1] = c1;
                    w[2] = c2;
                    w[3] = c3;
                    w += 4;
                }
                else
                {
                    uint32_t c[8];
                    for (unsigned j = 0; j < 8; j++)
                        c[j] = tbl[(b >> (7 - j)) & 1];
                    for (unsigned j = 0; j < 8; j++)
                        w[j] = c[j];
                    w += 8;
                }
            }
        else if (bpp == 8)
        {
#pragma GCC unroll 16
            for (; k + 4 <= row_bytes; k += 4, w += 2)
            {
                const uint32_t q = mode_load32(src + k);
                const uint32_t a = mode_pack2(pal[q & 0xFF], pal[(q >> 8) & 0xFF]);
                const uint32_t c = mode_pack2(pal[(q >> 16) & 0xFF], pal[q >> 24]);
                w[0] = a;
                w[1] = c;
            }
            for (; k < row_bytes; k += 2, w++)
            {
                const uint32_t a = mode_pack2(pal[src[k]], pal[src[k + 1]]);
                *w = a;
            }
        }
        else if (bpp == 4)
        {
#pragma GCC unroll 8
            for (; k + 4 <= row_bytes; k += 4, w += 4)
            {
                const uint32_t q = mode_load32(src + k);
                const uint32_t w0 = tbl[q & 0xFF], w1 = tbl[(q >> 8) & 0xFF];
                const uint32_t w2 = tbl[(q >> 16) & 0xFF], w3 = tbl[q >> 24];
                w[0] = w0;
                w[1] = w1;
                w[2] = w2;
                w[3] = w3;
            }
            for (; k < row_bytes; k++, w++)
                *w = tbl[src[k]];
        }
        else if (bpp == 2)
#pragma GCC unroll 16
            for (; k < row_bytes; k++, w += 2)
            {
                const uint32_t b = src[k];
                const uint32_t a = tbl[b >> 4], c = tbl[b & 0xF];
                w[0] = a;
                w[1] = c;
            }
        else
#pragma GCC unroll 8
            for (; k < row_bytes; k++, w += 4)
            {
                const uint32_t b = src[k];
                const uint32_t *hi = &tbl[(b >> 4) * 2];
                const uint32_t *lo = &tbl[(b & 0xF) * 2];
                const uint32_t w0 = hi[0], w1 = hi[1], w2 = lo[0], w3 = lo[1];
                w[0] = w0;
                w[1] = w1;
                w[2] = w2;
                w[3] = w3;
            }
    }
}

// The other tile shapes get a copy of the hot loop for every stored row
// length, so it is unrolled whole. A row of a custom tile 4 to 64 pixels wide,
// or of a standard tile that is trimmed or doubled, is (w + 1) * 4 pixels for
// w of 0 to 15. The step on the canvas stays a variable.
typedef void (*mode2_tiles_fn)(uint16_t *rgb, const uint8_t *map, unsigned count,
                               uint32_t ids, const uint8_t *tiles, uint32_t tile_bytes,
                               int16_t step, const uint32_t *tbl, const uint16_t *pal);

#define MODE2_TILES(BPP, HD, W)                                                     \
    static __attribute__((noinline)) void                                           \
    mode2_tiles_##BPP##_##HD##_##W(uint16_t *rgb, const uint8_t *map, unsigned count, \
                                   uint32_t ids, const uint8_t *tiles,               \
                                   uint32_t tile_bytes, int16_t step,                \
                                   const uint32_t *tbl, const uint16_t *pal)         \
    {                                                                               \
        mode2_emit_tiles(rgb, map, count, ids, tiles, tile_bytes,                   \
                         ((W + 1) * 4 * BPP + 7) / 8, step, tbl, pal, BPP, HD);     \
    }
#define MODE2_TILES_W(BPP, HD)                                                     \
    MODE2_TILES(BPP, HD, 0)                                                         \
    MODE2_TILES(BPP, HD, 1)                                                         \
    MODE2_TILES(BPP, HD, 2)                                                         \
    MODE2_TILES(BPP, HD, 3)                                                         \
    MODE2_TILES(BPP, HD, 4)                                                         \
    MODE2_TILES(BPP, HD, 5)                                                         \
    MODE2_TILES(BPP, HD, 6)                                                         \
    MODE2_TILES(BPP, HD, 7)                                                         \
    MODE2_TILES(BPP, HD, 8)                                                         \
    MODE2_TILES(BPP, HD, 9)                                                         \
    MODE2_TILES(BPP, HD, 10)                                                        \
    MODE2_TILES(BPP, HD, 11)                                                        \
    MODE2_TILES(BPP, HD, 12)                                                        \
    MODE2_TILES(BPP, HD, 13)                                                        \
    MODE2_TILES(BPP, HD, 14)                                                        \
    MODE2_TILES(BPP, HD, 15)
#define MODE2_TILES_ROW(BPP, HD)                                                    \
    {mode2_tiles_##BPP##_##HD##_0, mode2_tiles_##BPP##_##HD##_1,                    \
     mode2_tiles_##BPP##_##HD##_2, mode2_tiles_##BPP##_##HD##_3,                    \
     mode2_tiles_##BPP##_##HD##_4, mode2_tiles_##BPP##_##HD##_5,                    \
     mode2_tiles_##BPP##_##HD##_6, mode2_tiles_##BPP##_##HD##_7,                    \
     mode2_tiles_##BPP##_##HD##_8, mode2_tiles_##BPP##_##HD##_9,                    \
     mode2_tiles_##BPP##_##HD##_10, mode2_tiles_##BPP##_##HD##_11,                  \
     mode2_tiles_##BPP##_##HD##_12, mode2_tiles_##BPP##_##HD##_13,                  \
     mode2_tiles_##BPP##_##HD##_14, mode2_tiles_##BPP##_##HD##_15}
MODE2_TILES_W(1, 0)
MODE2_TILES_W(1, 1)
MODE2_TILES_W(2, 0)
MODE2_TILES_W(2, 1)
MODE2_TILES_W(4, 0)
MODE2_TILES_W(4, 1)
MODE2_TILES_W(8, 0)
MODE2_TILES_W(8, 1)

static const mode2_tiles_fn mode2_tiles[4][2][16] = {
    {MODE2_TILES_ROW(1, 0), MODE2_TILES_ROW(1, 1)},
    {MODE2_TILES_ROW(2, 0), MODE2_TILES_ROW(2, 1)},
    {MODE2_TILES_ROW(4, 0), MODE2_TILES_ROW(4, 1)},
    {MODE2_TILES_ROW(8, 0), MODE2_TILES_ROW(8, 1)},
};

// The map is walked a tile at a time. Whole tiles go to mode2_emit_tiles while
// the whole of their decoded row fits in the line; the tiles cut by an edge,
// and the last few before the end of the line, go through mode2_emit_part.
// bpp is a constant at every call, and for the standard 8x8 and 16x16 tiles
// shown whole, so is the geometry, so those fold to shifts and fixed loops.
// Every other shape passes its copy of the whole tile loop in tiles_fn.
static inline __attribute__((always_inline)) void
mode2_emit(mode2_config_t *config, uint16_t *rgb, int16_t width,
           const uint8_t *row_data, int16_t py, const uint16_t *pal,
           const uint32_t *pairs, const mode2_geom_t *g, mode2_tiles_fn tiles_fn, int16_t bpp)
{
    const int16_t scr_w = g->scr_w;
    const bool hdouble = g->hdouble;
    const uint32_t row_bytes = g->row_bytes;
    const uint32_t tile_bytes = g->tile_bytes;
    // The pixels a tile's whole stored row writes.
    const unsigned span = (unsigned)(row_bytes * 8 / bpp) << hdouble;
    const uint32_t base = (uint32_t)config->xram_tile_ptr + row_bytes * py;
    // The tile IDs whose row this scanline lies wholly in XRAM.
    uint32_t ids = 0;
    if (base + row_bytes <= 0x10000)
        ids = (0x10000 - row_bytes - base) / tile_bytes + 1;
    const uint8_t *tiles = (const uint8_t *)&xram[base & 0xFFFF];
    uint16_t *const line_end = rgb + width;
    MODE_TABLE(small, 32);
    const uint32_t *tbl = bpp == 4 && !hdouble ? pairs : small;
    if (hdouble)
        for (int i = 0; i < (bpp <= 2 ? 1 << bpp : 0); i++)
            small[i] = pal[i] * 0x10001u;
    else if (bpp == 1)
        mode_nibble_set(small, pal[0], pal[1]);
    else if (bpp == 2)
        mode_quad_set(small, pal);
    int16_t col = -config->x_pos_px;
    while (width)
    {
        int16_t fill_cols = mode2_fill_cols(config, &rgb, &col, &width, scr_w);
        if (fill_cols <= 0)
            continue;
        const uint8_t *map = row_data + col / scr_w;
        const unsigned x = (unsigned)(col % scr_w);
        col += fill_cols;
        if (x)
        {
            unsigned n = (unsigned)scr_w - x;
            if (n > (unsigned)fill_cols)
                n = fill_cols;
            mode2_emit_part(rgb, *map++, ids, tiles, tile_bytes, x, n, hdouble, pal, bpp);
            rgb += n;
            fill_cols -= n;
        }
        unsigned count = (unsigned)fill_cols / scr_w;
        fill_cols -= count * scr_w;
        // Whole tiles whose decoded row ends inside the line.
        const unsigned room = (unsigned)(line_end - rgb);
        unsigned fast = room >= span ? (room - span) / scr_w + 1 : 0;
        if (fast > count)
            fast = count;
        if (!tiles_fn)
            mode2_emit_tiles(rgb, map, fast, ids, tiles, tile_bytes, row_bytes, scr_w,
                             tbl, pal, bpp, false);
        else if (fast)
            tiles_fn(rgb, map, fast, ids, tiles, tile_bytes, scr_w, tbl, pal);
        rgb += fast * scr_w;
        map += fast;
        for (count -= fast; count; count--, rgb += scr_w)
            mode2_emit_part(rgb, *map++, ids, tiles, tile_bytes, 0, scr_w, hdouble, pal, bpp);
        if (fill_cols)
        {
            mode2_emit_part(rgb, *map, ids, tiles, tile_bytes, 0, fill_cols, hdouble, pal, bpp);
            rgb += fill_cols;
        }
    }
}

static inline __attribute__((always_inline)) bool
mode2_render_bpp(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr,
                 uint16_t opt, vga_fill_cache_t *cache, int16_t bpp)
{
    mode2_config_t cfg = *(const mode2_config_t *)&xram[config_ptr];
    mode2_config_t *config = &cfg;
    mode2_geom_t g;
    mode2_geometry(opt, bpp, &g);
    int16_t row;
    const uint8_t *row_data =
        mode2_scanline_to_data(scanline_id, width, config, g.scr_w, g.scr_h, &row);
    if (!row_data)
        return false;
    const int16_t py = (int16_t)(row >> g.vdouble);
    const uint16_t *palette = mode2_get_palette(config, bpp);
    // The two small palettes are copied, so that a write to XRAM part way
    // through the line changes no pixel of it.
    uint16_t pal[4];
    if (bpp <= 2)
        for (int i = 0; i < (1 << bpp); i++)
            pal[i] = palette[i];
    const uint16_t *p = bpp <= 2 ? pal : palette;
    // 4bpp tiles not doubled across read each byte's two pixels at once.
    const uint32_t *pairs = bpp == 4 && !g.hdouble ? mode_4bpp_pairs(cache, palette, false) : NULL;
    // The standard tiles, shown whole, get geometry the compiler can see.
    if (!(opt & (MODE2_CUSTOM | MODE2_HDOUBLE | 0x00F0)))
    {
        mode2_geom_t fixed = g;
        if (opt & 0x08)
        {
            fixed.tex_w = fixed.scr_w = 16;
            fixed.row_bytes = 16 * bpp / 8;
            fixed.tile_bytes = 16 * 16 * bpp / 8;
            mode2_emit(config, rgb, width, row_data, py, p, pairs, &fixed, NULL, bpp);
        }
        else
        {
            fixed.tex_w = fixed.scr_w = 8;
            fixed.row_bytes = 8 * bpp / 8;
            fixed.tile_bytes = 8 * 8 * bpp / 8;
            mode2_emit(config, rgb, width, row_data, py, p, pairs, &fixed, NULL, bpp);
        }
    }
    else
    {
        const int bi = bpp == 1 ? 0 : bpp == 2 ? 1 : bpp == 4 ? 2 : 3;
        const unsigned w = (opt & MODE2_CUSTOM) ? (opt >> 4) & 0x0F : (opt & 0x08) ? 3 : 1;
        mode2_emit(config, rgb, width, row_data, py, p, pairs, &g, mode2_tiles[bi][g.hdouble][w], bpp);
    }
    return true;
}

static bool
mode2_render(int16_t plane_id, int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr,
             vga_fill_cache_t *cache)
{
    const uint16_t opt = mode2_options[scanline_id][plane_id];
    switch (opt & 0x07)
    {
    case 0:
        return mode2_render_bpp(scanline_id, width, rgb, config_ptr, opt, cache, 1);
    case 1:
        return mode2_render_bpp(scanline_id, width, rgb, config_ptr, opt, cache, 2);
    case 2:
        return mode2_render_bpp(scanline_id, width, rgb, config_ptr, opt, cache, 4);
    case 3:
        return mode2_render_bpp(scanline_id, width, rgb, config_ptr, opt, cache, 8);
    default:
        return false;
    }
}

vga_fill_fn_t mode2_fill_fn(uint16_t attributes)
{
    return (attributes & 0x8000) ? NULL : mode2_render;
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
    if (options & 0x8000 || (options & 0x07) > 3 ||
        plane < 0 || plane >= SCANVIDEO_PLANE_COUNT)
        return false;
    // A custom size takes any value in bits 4:11. A trim must leave at least
    // one column and one row of the tile.
    const int16_t tile_size = (options & 0x08) ? 16 : 8;
    const int16_t x_trim = (options >> 4) & 0x0F;
    const int16_t y_trim = (options >> 8) & 0x0F;
    if (!(options & MODE2_CUSTOM) && (x_trim >= tile_size || y_trim >= tile_size))
        return false;

    const int16_t end = scanline_end ? scanline_end : vga_canvas_height();
    if (!vga_prog_fill(plane, scanline_begin, end, config_ptr, mode2_render))
        return false;
    for (int16_t i = scanline_begin; i < end; i++)
        mode2_options[i][plane] = options;
    return true;
}

#pragma GCC pop_options
