/*
 * Copyright (c) 2026 Rumbledethumps
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// This is compatible with the sprite system in pico-playground which
// is based on the sprite system used for the RISCBoy games console.

#include "modes/mode4.h"
#include "sys/mem.h"
#include "sys/vga.h"
#include <hardware/interp.h>

#pragma GCC push_options
#pragma GCC optimize("O3")

typedef struct
{
    int16_t x_pos_px;
    int16_t y_pos_px;
    uint16_t xram_sprite_ptr;
    uint8_t log_size;
    bool has_opacity_metadata;
} mode4_sprite_t;

// transform[6] is the top two rows of the affine matrix in signed 8.8 fixed
// point, row-major: { a00, a01, b0, a10, a11, b1 }. Sign-extended and shifted
// to signed 16.16 before use.
typedef struct
{
    int16_t transform[6];
    int16_t x_pos_px;
    int16_t y_pos_px;
    uint16_t xram_sprite_ptr;
    uint8_t log_size;
    bool has_opacity_metadata;
} mode4_asprite_t;

// Unpacked affine transform: { a00, a01, b0, a10, a11, b1 } in signed 16.16.
// [u]   [ a00 a01 b0 ]   [x]   [a00 * x + a01 * y + b0]
// [v] = [ a10 a11 b1 ] * [y] = [a10 * x + a11 * y + b1]
// [1]   [ 0   0   1  ]   [1]   [           1          ]
typedef int32_t affine_transform_t[6];
static const int32_t AF_ONE = 1 << 16;

static inline int32_t mul_fp1616(int32_t x, int32_t y)
{
    int64_t result = (int64_t)x * y;
    return result >> 16;
}

typedef struct
{
    int tex_offs_x;
    int tex_offs_y;
    int size_x;
} intersect_t;

// Always-inline else the compiler does rash things like passing structs in memory
static inline __attribute__((always_inline)) intersect_t
get_sprite_intersect(int x_pos_px, int y_pos_px, int log_size, uint raster_y, uint raster_w)
{
    intersect_t isct = {0};
    isct.tex_offs_y = (int)raster_y - y_pos_px;
    int size = 1u << log_size;
    uint upper_mask = -size;
    if ((uint)isct.tex_offs_y & upper_mask)
        return isct;
    int x_start_clipped = MAX(0, x_pos_px);
    isct.tex_offs_x = x_start_clipped - x_pos_px;
    isct.size_x = MIN(x_pos_px + size, (int)raster_w) - x_start_clipped;
    return isct;
}

// Sprites may have an array of metadata on the end.
// One word per line, encodes first opaque pixel, last opaque pixel,
// and whether the span in between is solid. This allows fewer pixel-by-pixel alpha tests.
static inline intersect_t intersect_with_metadata(intersect_t isct, uint32_t meta)
{
    int span_end = meta & 0xffff;
    int span_start = (meta >> 16) & 0x7fff;
    int isct_new_start = MAX(isct.tex_offs_x, span_start);
    int isct_new_end = MIN(isct.tex_offs_x + isct.size_x, span_end);
    isct.tex_offs_x = isct_new_start;
    isct.size_x = isct_new_end - isct_new_start;
    return isct;
}

static inline void sprite_blit16(uint16_t *dst, const uint16_t *src, uint len)
{
    uint16_t *dst_start = dst;
    uint pixels_8 = (len >> 3) << 3;
    uint remainder = len & 7;
    dst += pixels_8;
    src += pixels_8;
    switch (remainder)
    {
    case 7:
        dst[6] = src[6];
        __attribute__((fallthrough));
    case 6:
        dst[5] = src[5];
        __attribute__((fallthrough));
    case 5:
        dst[4] = src[4];
        __attribute__((fallthrough));
    case 4:
        dst[3] = src[3];
        __attribute__((fallthrough));
    case 3:
        dst[2] = src[2];
        __attribute__((fallthrough));
    case 2:
        dst[1] = src[1];
        __attribute__((fallthrough));
    case 1:
        dst[0] = src[0];
        __attribute__((fallthrough));
    case 0:
        break;
    }
    if (dst <= dst_start)
        return;
    do
    {
        dst -= 8;
        src -= 8;
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
        dst[4] = src[4];
        dst[5] = src[5];
        dst[6] = src[6];
        dst[7] = src[7];
    } while (dst > dst_start);
}

static inline void sprite_blit16_alpha(uint16_t *dst, const uint16_t *src, uint len)
{
    uint16_t *dst_start = dst;
    uint pixels_8 = (len >> 3) << 3;
    uint remainder = len & 7;
    dst += pixels_8;
    src += pixels_8;
    switch (remainder)
    {
    case 7:
        if (src[6] & (1 << 5))
            dst[6] = src[6];
        __attribute__((fallthrough));
    case 6:
        if (src[5] & (1 << 5))
            dst[5] = src[5];
        __attribute__((fallthrough));
    case 5:
        if (src[4] & (1 << 5))
            dst[4] = src[4];
        __attribute__((fallthrough));
    case 4:
        if (src[3] & (1 << 5))
            dst[3] = src[3];
        __attribute__((fallthrough));
    case 3:
        if (src[2] & (1 << 5))
            dst[2] = src[2];
        __attribute__((fallthrough));
    case 2:
        if (src[1] & (1 << 5))
            dst[1] = src[1];
        __attribute__((fallthrough));
    case 1:
        if (src[0] & (1 << 5))
            dst[0] = src[0];
        __attribute__((fallthrough));
    case 0:
        break;
    }
    if (dst <= dst_start)
        return;
    do
    {
        dst -= 8;
        src -= 8;
        if (src[0] & (1 << 5))
            dst[0] = src[0];
        if (src[1] & (1 << 5))
            dst[1] = src[1];
        if (src[2] & (1 << 5))
            dst[2] = src[2];
        if (src[3] & (1 << 5))
            dst[3] = src[3];
        if (src[4] & (1 << 5))
            dst[4] = src[4];
        if (src[5] & (1 << 5))
            dst[5] = src[5];
        if (src[6] & (1 << 5))
            dst[6] = src[6];
        if (src[7] & (1 << 5))
            dst[7] = src[7];
    } while (dst > dst_start);
}

static inline void sprite_scanline16(
    uint16_t *scanbuf, const mode4_sprite_t *sp, const void *sp_img, uint raster_y, uint raster_w)
{
    int size = 1u << sp->log_size;
    intersect_t isct = get_sprite_intersect(sp->x_pos_px, sp->y_pos_px, sp->log_size, raster_y, raster_w);
    if (isct.size_x <= 0)
        return;
    const uint16_t *img = sp_img;
    bool span_continuous = false;
    if (sp->has_opacity_metadata)
    {
        uint32_t meta = ((uint32_t *)(sp_img + size * size * sizeof(uint16_t)))[isct.tex_offs_y];
        isct = intersect_with_metadata(isct, meta);
        if (isct.size_x <= 0)
            return;
        span_continuous = !!(meta & (1u << 31));
    }
    uint16_t *dst = scanbuf + sp->x_pos_px + isct.tex_offs_x;
    const uint16_t *src = img + isct.tex_offs_x + isct.tex_offs_y * size;
    if (span_continuous)
        sprite_blit16(dst, src, isct.size_x);
    else
        sprite_blit16_alpha(dst, src, isct.size_x);
}

static void mode4_render_sprite(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    const mode4_sprite_t *sprites = (void *)&xram[config_ptr];
    for (uint16_t i = 0; i < length; i++)
    {
        const unsigned px_size = 1u << sprites[i].log_size;
        unsigned byte_size = px_size * px_size * sizeof(uint16_t);
        if (sprites[i].has_opacity_metadata)
            byte_size += px_size * sizeof(uint32_t);
        if (byte_size > 0x10000)
            continue;
        if (sprites[i].xram_sprite_ptr <= 0x10000 - byte_size)
        {
            const void *img = (void *)&xram[sprites[i].xram_sprite_ptr];
            sprite_scanline16(rgb, &sprites[i], img, scanline, width);
        }
    }
}

// Set up an interpolator to follow a straight line through u,v space
static inline void setup_interp_affine(
    intersect_t isct,
    const affine_transform_t atrans)
{
    // Calculate the u,v coord of the first sample. Note that we are iterating
    // *backward* along the raster span because this is faster (yes)
    int32_t x0 =
        mul_fp1616(atrans[0], (isct.tex_offs_x + isct.size_x) * AF_ONE) +
        mul_fp1616(atrans[1], isct.tex_offs_y * AF_ONE) +
        atrans[2];
    int32_t y0 =
        mul_fp1616(atrans[3], (isct.tex_offs_x + isct.size_x) * AF_ONE) +
        mul_fp1616(atrans[4], isct.tex_offs_y * AF_ONE) +
        atrans[5];
    interp0->accum[0] = x0;
    interp0->accum[1] = y0;
    interp0->base[0] = -atrans[0]; // -a00, since x decrements by 1 with each coord
    interp0->base[1] = -atrans[3]; // -a10
}

// Set up an interpolator to generate pixel lookup addresses from fp1616
// numbers in accum1, accum0 based on the parameters of sprite sp and the size
// of the individual pixels
static inline void setup_interp_pix_coordgen(
    const mode4_asprite_t *sp,
    const void *sp_img, uint pixel_shift)
{
    // Concatenate from accum0[31:16] and accum1[31:16] as many LSBs as required
    // to index the sprite texture in both directions. Reading from POP_FULL will
    // yield these bits, added to sp->img, and this will also trigger BASE0 and
    // BASE1 to be directly added (thanks to CTRL_ADD_RAW) to the accumulators,
    // which generates the u,v coordinate for the *next* read.
    assert(sp->log_size + pixel_shift <= 15);

    interp_config c0 = interp_default_config();
    interp_config_set_add_raw(&c0, true);
    interp_config_set_shift(&c0, 16 - pixel_shift);
    interp_config_set_mask(&c0, pixel_shift, pixel_shift + sp->log_size - 1);
    interp_set_config(interp0, 0, &c0);

    interp_config c1 = interp_default_config();
    interp_config_set_add_raw(&c1, true);
    interp_config_set_shift(&c1, 16 - sp->log_size - pixel_shift);
    interp_config_set_mask(&c1, pixel_shift + sp->log_size, pixel_shift + 2 * sp->log_size - 1);
    interp_set_config(interp0, 1, &c1);

    interp_set_base(interp0, 2, (uint32_t)sp_img);
}

static inline void sprite_ablit16_alpha_loop_body(uint16_t *dst, uint mask)
{
    uint32_t overflow = (interp0->accum[0] | interp0->accum[1]) & mask;
    uint16_t *src_addr = (uint16_t *)interp0->pop[2];
    if (overflow)
        return;
    uint16_t pixel = *src_addr;
    if (pixel & (1 << 5)) // alpha
        *dst = pixel;
}

static inline void sprite_ablit16_alpha_loop(uint16_t *dst, uint len, uint mask)
{
    uint16_t *dst_start = dst;
    uint pixels_8 = (len >> 3) << 3;
    uint remainder = len & 7;
    dst += pixels_8;
    switch (remainder)
    {
    case 7:
        sprite_ablit16_alpha_loop_body(&dst[6], mask);
        __attribute__((fallthrough));
    case 6:
        sprite_ablit16_alpha_loop_body(&dst[5], mask);
        __attribute__((fallthrough));
    case 5:
        sprite_ablit16_alpha_loop_body(&dst[4], mask);
        __attribute__((fallthrough));
    case 4:
        sprite_ablit16_alpha_loop_body(&dst[3], mask);
        __attribute__((fallthrough));
    case 3:
        sprite_ablit16_alpha_loop_body(&dst[2], mask);
        __attribute__((fallthrough));
    case 2:
        sprite_ablit16_alpha_loop_body(&dst[1], mask);
        __attribute__((fallthrough));
    case 1:
        sprite_ablit16_alpha_loop_body(&dst[0], mask);
        __attribute__((fallthrough));
    case 0:
        break;
    }
    if (dst <= dst_start)
        return;
    do
    {
        dst -= 8;
        sprite_ablit16_alpha_loop_body(&dst[7], mask);
        sprite_ablit16_alpha_loop_body(&dst[6], mask);
        sprite_ablit16_alpha_loop_body(&dst[5], mask);
        sprite_ablit16_alpha_loop_body(&dst[4], mask);
        sprite_ablit16_alpha_loop_body(&dst[3], mask);
        sprite_ablit16_alpha_loop_body(&dst[2], mask);
        sprite_ablit16_alpha_loop_body(&dst[1], mask);
        sprite_ablit16_alpha_loop_body(&dst[0], mask);
    } while (dst > dst_start);
}

static inline void asprite_scanline16(
    uint16_t *scanbuf, const mode4_asprite_t *sp, const void *sp_img,
    uint raster_y, uint raster_w)
{
    intersect_t isct = get_sprite_intersect(sp->x_pos_px, sp->y_pos_px, sp->log_size, raster_y, raster_w);
    if (isct.size_x <= 0)
        return;
    affine_transform_t atrans;
    for (uint16_t j = 0; j < 6; j++)
        atrans[j] = (int32_t)sp->transform[j] << 8;
    setup_interp_affine(isct, atrans);
    setup_interp_pix_coordgen(sp, sp_img, 1);
    sprite_ablit16_alpha_loop(scanbuf + MAX(0, sp->x_pos_px), isct.size_x, 0xFFFF0000 << sp->log_size);
}

static void mode4_render_asprite(
    int16_t scanline, int16_t width, uint16_t *rgb,
    uint16_t config_ptr, uint16_t length)
{
    const mode4_asprite_t *sprites = (void *)&xram[config_ptr];
    for (uint16_t i = 0; i < length; i++)
    {
        const unsigned px_size = 1u << sprites[i].log_size;
        unsigned byte_size = px_size * px_size * sizeof(uint16_t);
        if (sprites[i].has_opacity_metadata)
            byte_size += px_size * sizeof(uint32_t);
        if (byte_size > 0x10000)
            continue;
        if (sprites[i].xram_sprite_ptr <= 0x10000 - byte_size)
        {
            const void *img = (void *)&xram[sprites[i].xram_sprite_ptr];
            asprite_scanline16(rgb, &sprites[i], img, scanline, width);
        }
    }
}

bool mode4_prog(uint16_t *xregs)
{
    const uint16_t attributes = xregs[2];
    const uint16_t config_ptr = xregs[3];
    const uint16_t length = xregs[4];
    const int16_t plane = xregs[5];
    const int16_t scanline_begin = xregs[6];
    const int16_t scanline_end = xregs[7];

    if (config_ptr & 1)
        return false;

    void (*render_fn)(int16_t, int16_t, uint16_t *, uint16_t, uint16_t);
    switch (attributes)
    {
    case 0:
    {
        render_fn = mode4_render_sprite;
        const uint32_t region_size = (uint32_t)sizeof(mode4_sprite_t) * length;
        if (region_size > 0x10000 || config_ptr > 0x10000 - region_size)
            return false;
        break;
    }
    case 1:
    {
        render_fn = mode4_render_asprite;
        const uint32_t region_size = (uint32_t)sizeof(mode4_asprite_t) * length;
        if (region_size > 0x10000 || config_ptr > 0x10000 - region_size)
            return false;
        break;
    }
    default:
        return false;
    }

    return vga_prog_sprite(plane, scanline_begin, scanline_end, config_ptr, length, render_fn);
}

#pragma GCC pop_options
