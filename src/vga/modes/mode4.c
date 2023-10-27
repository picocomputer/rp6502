/*
 * Copyright (c) 2023 Rumbledethumps
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// This is the sprite code from pico-playground.
// The affine transform code came with this comment:
// "Stolen from RISCBoy"

#include "modes/mode4.h"
#include "sys/mem.h"
#include "sys/vga.h"
#include "pico/scanvideo.h"
#include "hardware/interp.h"
#include <stdint.h>

typedef struct
{
    int16_t x_pos_px;
    int16_t y_pos_px;
    uint16_t xram_sprite_ptr;
    uint8_t log_size;
    bool has_opacity_metadata;
} mode4_sprite_t;

typedef struct
{
    int16_t transform[6];
    int16_t x_pos_px;
    int16_t y_pos_px;
    uint16_t xram_sprite_ptr;
    uint8_t log_size;
    bool has_opacity_metadata;
} mode4_asprite_t;

// Note some of the sprite routines are quite large (unrolled), so trying to
// keep everything in separate sections so the linker can garbage collect
// unused sprite code. In particular we usually need 8bpp xor 16bpp functions!
#define __ram_func(foo) __not_in_flash_func(foo)

// ----------------------------------------------------------------------------
// Functions from sprite.S

// Constant-colour span
void sprite_fill16(uint16_t *dst, uint16_t colour, uint len);

// Block image transfers
void sprite_blit16(uint16_t *dst, const uint16_t *src, uint len);
void sprite_blit16_alpha(uint16_t *dst, const uint16_t *src, uint len);

// These are just inner loops, and require INTERP0 to be configured before calling:
void sprite_ablit16_loop(uint16_t *dst, uint len);
void sprite_ablit16_alpha_loop(uint16_t *dst, uint len);

// Store unpacked affine transforms as signed 16.16 fixed point in the following order:
// a00, a01, b0,   a10, a11, b1
// i.e. the top two rows of the matrix
// [ a00 a01 b0 ]
// [ a01 a11 b1 ]
// [ 0   0   1  ]
// Then pack integers appropriately

typedef int32_t affine_transform_t[6];
static const int32_t AF_ONE = 1 << 16;

static inline __attribute__((always_inline)) int32_t mul_fp1616(int32_t x, int32_t y)
{
    // TODO this results in an aeabi call?!
    int64_t result = (int64_t)x * y;
    return result >> 16;
}

// result can not be == left or right
static inline void affine_mul(affine_transform_t result, const affine_transform_t left,
                              const affine_transform_t right)
{
    result[0] = mul_fp1616(left[0], right[0]) + mul_fp1616(left[1], right[3]);
    result[1] = mul_fp1616(left[0], right[1]) + mul_fp1616(left[1], right[4]);
    result[2] = mul_fp1616(left[0], right[2]) + mul_fp1616(left[1], right[5]) + left[2];
    result[3] = mul_fp1616(left[3], right[0]) + mul_fp1616(left[4], right[3]);
    result[4] = mul_fp1616(left[3], right[1]) + mul_fp1616(left[4], right[4]);
    result[5] = mul_fp1616(left[3], right[2]) + mul_fp1616(left[4], right[5]) + left[5];
}

static inline void affine_copy(affine_transform_t dst, const affine_transform_t src)
{
    for (int i = 0; i < 6; ++i)
        dst[i] = src[i];
}

// User is describing a sequence of transformations from texture space to
// screen space, which are applied by premultiplying a column vector. However,
// hardware transforms *from* screenspace *to* texture space, so we want the
// inverse of the transform the user is building. Therefore our functions each
// produce the inverse of the requested transform, and we apply transforms by
// *post*-multiplication.
static inline void affine_identity(affine_transform_t current_trans)
{
    int32_t tmp[6] = {
        AF_ONE, 0, 0,
        0, AF_ONE, 0};
    affine_copy(current_trans, tmp);
}

static inline void affine_translate(affine_transform_t current_trans, int32_t x, int32_t y)
{
    int32_t tmp[6];
    int32_t transform[6] = {
        AF_ONE, 0, -AF_ONE * x,
        0, AF_ONE, -AF_ONE * y};
    affine_mul(tmp, current_trans, transform);
    affine_copy(current_trans, tmp);
}

// Inherited comment:
// TODO this is shit
static const int32_t __not_in_flash("atrans") sin_lookup_fp1616[256] = {
    0x0, 0x648, 0xc8f, 0x12d5, 0x1917, 0x1f56, 0x2590, 0x2bc4, 0x31f1, 0x3817,
    0x3e33, 0x4447, 0x4a50, 0x504d, 0x563e, 0x5c22, 0x61f7, 0x67bd, 0x6d74,
    0x7319, 0x78ad, 0x7e2e, 0x839c, 0x88f5, 0x8e39, 0x9368, 0x987f, 0x9d7f,
    0xa267, 0xa736, 0xabeb, 0xb085, 0xb504, 0xb968, 0xbdae, 0xc1d8, 0xc5e4,
    0xc9d1, 0xcd9f, 0xd14d, 0xd4db, 0xd848, 0xdb94, 0xdebe, 0xe1c5, 0xe4aa,
    0xe76b, 0xea09, 0xec83, 0xeed8, 0xf109, 0xf314, 0xf4fa, 0xf6ba, 0xf853,
    0xf9c7, 0xfb14, 0xfc3b, 0xfd3a, 0xfe13, 0xfec4, 0xff4e, 0xffb1, 0xffec,
    0x10000, 0xffec, 0xffb1, 0xff4e, 0xfec4, 0xfe13, 0xfd3a, 0xfc3b, 0xfb14,
    0xf9c7, 0xf853, 0xf6ba, 0xf4fa, 0xf314, 0xf109, 0xeed8, 0xec83, 0xea09,
    0xe76b, 0xe4aa, 0xe1c5, 0xdebe, 0xdb94, 0xd848, 0xd4db, 0xd14d, 0xcd9f,
    0xc9d1, 0xc5e4, 0xc1d8, 0xbdae, 0xb968, 0xb504, 0xb085, 0xabeb, 0xa736,
    0xa267, 0x9d7f, 0x987f, 0x9368, 0x8e39, 0x88f5, 0x839c, 0x7e2e, 0x78ad,
    0x7319, 0x6d74, 0x67bd, 0x61f7, 0x5c22, 0x563e, 0x504d, 0x4a50, 0x4447,
    0x3e33, 0x3817, 0x31f1, 0x2bc4, 0x2590, 0x1f56, 0x1917, 0x12d5, 0xc8f, 0x648,
    0x0, 0xfffff9b8, 0xfffff371, 0xffffed2b, 0xffffe6e9, 0xffffe0aa, 0xffffda70,
    0xffffd43c, 0xffffce0f, 0xffffc7e9, 0xffffc1cd, 0xffffbbb9, 0xffffb5b0,
    0xffffafb3, 0xffffa9c2, 0xffffa3de, 0xffff9e09, 0xffff9843, 0xffff928c,
    0xffff8ce7, 0xffff8753, 0xffff81d2, 0xffff7c64, 0xffff770b, 0xffff71c7,
    0xffff6c98, 0xffff6781, 0xffff6281, 0xffff5d99, 0xffff58ca, 0xffff5415,
    0xffff4f7b, 0xffff4afc, 0xffff4698, 0xffff4252, 0xffff3e28, 0xffff3a1c,
    0xffff362f, 0xffff3261, 0xffff2eb3, 0xffff2b25, 0xffff27b8, 0xffff246c,
    0xffff2142, 0xffff1e3b, 0xffff1b56, 0xffff1895, 0xffff15f7, 0xffff137d,
    0xffff1128, 0xffff0ef7, 0xffff0cec, 0xffff0b06, 0xffff0946, 0xffff07ad,
    0xffff0639, 0xffff04ec, 0xffff03c5, 0xffff02c6, 0xffff01ed, 0xffff013c,
    0xffff00b2, 0xffff004f, 0xffff0014, 0xffff0000, 0xffff0014, 0xffff004f,
    0xffff00b2, 0xffff013c, 0xffff01ed, 0xffff02c6, 0xffff03c5, 0xffff04ec,
    0xffff0639, 0xffff07ad, 0xffff0946, 0xffff0b06, 0xffff0cec, 0xffff0ef7,
    0xffff1128, 0xffff137d, 0xffff15f7, 0xffff1895, 0xffff1b56, 0xffff1e3b,
    0xffff2142, 0xffff246c, 0xffff27b8, 0xffff2b25, 0xffff2eb3, 0xffff3261,
    0xffff362f, 0xffff3a1c, 0xffff3e28, 0xffff4252, 0xffff4698, 0xffff4afc,
    0xffff4f7b, 0xffff5415, 0xffff58ca, 0xffff5d99, 0xffff6281, 0xffff6781,
    0xffff6c98, 0xffff71c7, 0xffff770b, 0xffff7c64, 0xffff81d2, 0xffff8753,
    0xffff8ce7, 0xffff928c, 0xffff9843, 0xffff9e09, 0xffffa3de, 0xffffa9c2,
    0xffffafb3, 0xffffb5b0, 0xffffbbb9, 0xffffc1cd, 0xffffc7e9, 0xffffce0f,
    0xffffd43c, 0xffffda70, 0xffffe0aa, 0xffffe6e9, 0xffffed2b, 0xfffff371,
    0xfffff9b8};

static inline int32_t sin_fp1616(uint8_t theta)
{
    return sin_lookup_fp1616[theta];
}

static inline int32_t cos_fp1616(uint8_t theta)
{
    return sin_lookup_fp1616[(theta + 64) & 0xff];
}

// Appears as a counterclockwise rotation (when viewed from texture space to screen space)
// Units of angle are 256 = one turn
static inline void affine_rotate(affine_transform_t current_trans, uint8_t theta)
{
    int32_t tmp[6];
    int32_t transform[6] = {
        cos_fp1616(theta), -sin_fp1616(theta), 0,
        sin_fp1616(theta), cos_fp1616(theta), 0};
    affine_mul(tmp, current_trans, transform);
    affine_copy(current_trans, tmp);
}

static inline void affine_scale(affine_transform_t current_trans, int32_t sx, int32_t sy)
{
    int32_t sx_inv = ((int64_t)AF_ONE * AF_ONE) / sx;
    int32_t sy_inv = ((int64_t)AF_ONE * AF_ONE) / sy;
    int32_t tmp[6];
    int32_t transform[6] = {
        sx_inv, 0, 0,
        0, sy_inv, 0};
    affine_mul(tmp, current_trans, transform);
    affine_copy(current_trans, tmp);
}

typedef struct
{
    int tex_offs_x;
    int tex_offs_y;
    int size_x;
} intersect_t;

// Always-inline else the compiler does rash things like passing structs in memory
static inline intersect_t _get_asprite_intersect(const mode4_asprite_t *sp, uint raster_y, uint raster_w)
{
    intersect_t isct = {0};
    isct.tex_offs_y = (int)raster_y - sp->y_pos_px;
    int size = 1u << sp->log_size;
    uint upper_mask = -size;
    if ((uint)isct.tex_offs_y & upper_mask)
        return isct;
    int x_start_clipped = MAX(0, sp->x_pos_px);
    isct.tex_offs_x = x_start_clipped - sp->x_pos_px;
    isct.size_x = MIN(sp->x_pos_px + size, (int)raster_w) - x_start_clipped;
    return isct;
}

// Always-inline else the compiler does rash things like passing structs in memory
static inline intersect_t _get_sprite_intersect(const mode4_sprite_t *sp, uint raster_y, uint raster_w)
{
    intersect_t isct = {0};
    isct.tex_offs_y = (int)raster_y - sp->y_pos_px;
    int size = 1u << sp->log_size;
    uint upper_mask = -size;
    if ((uint)isct.tex_offs_y & upper_mask)
        return isct;
    int x_start_clipped = MAX(0, sp->x_pos_px);
    isct.tex_offs_x = x_start_clipped - sp->x_pos_px;
    isct.size_x = MIN(sp->x_pos_px + size, (int)raster_w) - x_start_clipped;
    return isct;
}

// Sprites may have an array of metadata on the end.
// One word per line, encodes first opaque pixel, last opaque pixel,
// and whether the span in between is solid. This allows fewer
static inline intersect_t _intersect_with_metadata(intersect_t isct, uint32_t meta)
{
    int span_end = meta & 0xffff;
    int span_start = (meta >> 16) & 0x7fff;
    int isct_new_start = MAX(isct.tex_offs_x, span_start);
    int isct_new_end = MIN(isct.tex_offs_x + isct.size_x, span_end);
    isct.tex_offs_x = isct_new_start;
    isct.size_x = isct_new_end - isct_new_start;
    return isct;
}

static inline __attribute__((always_inline)) void __ram_func(sprite_sprite16)(
    uint16_t *scanbuf, const mode4_sprite_t *sp, const void *sp_img, uint raster_y, uint raster_w)
{
    int size = 1u << sp->log_size;
    intersect_t isct = _get_sprite_intersect(sp, raster_y, raster_w);
    if (isct.size_x <= 0)
        return;
    const uint16_t *img = sp_img;
    if (sp->has_opacity_metadata)
    {
        uint32_t meta = ((uint32_t *)(sp_img + size * size * sizeof(uint16_t)))[isct.tex_offs_y];
        isct = _intersect_with_metadata(isct, meta);
        if (isct.size_x <= 0)
            return;
        bool span_continuous = !!(meta & (1u << 31));
        if (span_continuous)
            sprite_blit16(scanbuf + sp->x_pos_px + isct.tex_offs_x, img + isct.tex_offs_x + isct.tex_offs_y * size,
                          isct.size_x);
        else
            sprite_blit16_alpha(scanbuf + sp->x_pos_px + isct.tex_offs_x, img + isct.tex_offs_x + isct.tex_offs_y * size,
                                isct.size_x);
    }
    else
    {
        sprite_blit16_alpha(scanbuf + MAX(0, sp->x_pos_px), img + isct.tex_offs_x + isct.tex_offs_y * size, isct.size_x);
    }
}

static void mode4_render_sprite(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    const mode4_sprite_t *sprites = (void *)&xram[config_ptr];
    for (uint16_t i; i < length; i++)
    {
        const unsigned px_size = 2 ^ sprites[i].log_size;
        unsigned byte_size = px_size * px_size * sizeof(uint16_t);
        if (sprites[i].has_opacity_metadata)
            byte_size += px_size * sizeof(uint32_t);
        if (sprites[i].xram_sprite_ptr <= 0x10000 - byte_size)
        {
            const void *img = (void *)&xram[sprites[i].xram_sprite_ptr];
            sprite_sprite16(rgb, &sprites[i], img, scanline, width);
        }
    }
}

// We're defining the affine transform as:
//
// [u]   [ a00 a01 b0 ]   [x]   [a00 * x + a01 * y + b0]
// [v] = [ a10 a11 b1 ] * [y] = [a10 * x + a11 * y + b1]
// [1]   [ 0   0   1  ]   [1]   [           1          ]
//
// We represent this in memory as {a00, a01, b0, a10, a11, b1} (all int32_t)
// i.e. the non-constant parts in row-major order

// Set up an interpolator to follow a straight line through u,v space
static inline __attribute__((always_inline)) void _setup_interp_affine(interp_hw_t *interp, intersect_t isct,
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
    interp->accum[0] = x0;
    interp->accum[1] = y0;
    interp->base[0] = -atrans[0]; // -a00, since x decrements by 1 with each coord
    interp->base[1] = -atrans[3]; // -a10
}

// Set up an interpolator to generate pixel lookup addresses from fp1616
// numbers in accum1, accum0 based on the parameters of sprite sp and the size
// of the individual pixels
static inline __attribute__((always_inline)) void _setup_interp_pix_coordgen(
    interp_hw_t *interp, const mode4_asprite_t *sp, const void *sp_img, uint pixel_shift)
{
    // Concatenate from accum0[31:16] and accum1[31:16] as many LSBs as required
    // to index the sprite texture in both directions. Reading from POP_FULL will
    // yields these bits, added to sp->img, and this will also trigger BASE0 and
    // BASE1 to be directly added (thanks to CTRL_ADD_RAW) to the accumulators,
    // which generates the u,v coordinate for the *next* read.
    assert(sp->log_size + pixel_shift <= 16);

    interp_config c0 = interp_default_config();
    interp_config_set_add_raw(&c0, true);
    interp_config_set_shift(&c0, 16 - pixel_shift);
    interp_config_set_mask(&c0, pixel_shift, pixel_shift + sp->log_size - 1);
    interp_set_config(interp, 0, &c0);

    interp_config c1 = interp_default_config();
    interp_config_set_add_raw(&c1, true);
    interp_config_set_shift(&c1, 16 - sp->log_size - pixel_shift);
    interp_config_set_mask(&c1, pixel_shift + sp->log_size, pixel_shift + 2 * sp->log_size - 1);
    interp_set_config(interp, 1, &c1);

    interp->base[2] = (uint32_t)sp_img;
}

// Note we do NOT save/restore the interpolator!
void __ram_func(sprite_asprite16)(
    uint16_t *scanbuf, const mode4_asprite_t *sp, const void *sp_img,
    uint raster_y, uint raster_w)
{
    intersect_t isct = _get_asprite_intersect(sp, raster_y, raster_w);
    if (isct.size_x <= 0)
        return;
    interp_hw_t *interp = interp0;
    affine_transform_t atrans;
    for (uint16_t j; j < 6; j++)
        atrans[j] = (int32_t)sp->transform[j] << 8;
    _setup_interp_affine(interp, isct, atrans);
    _setup_interp_pix_coordgen(interp, sp, sp_img, 1);
    sprite_ablit16_alpha_loop(scanbuf + MAX(0, sp->x_pos_px), isct.size_x);
}

static void mode4_render_asprite(int16_t scanline, int16_t width, uint16_t *rgb, uint16_t config_ptr, uint16_t length)
{
    mode4_asprite_t *sprites = (void *)&xram[config_ptr];
    for (uint16_t i; i < length; i++)
    {
        const unsigned px_size = 2 ^ sprites[i].log_size;
        unsigned byte_size = px_size * px_size * sizeof(uint16_t);
        if (sprites[i].has_opacity_metadata)
            byte_size += px_size * sizeof(uint32_t);
        if (sprites[i].xram_sprite_ptr <= 0x10000 - byte_size)
        {
            const void *img = (void *)&xram[sprites[i].xram_sprite_ptr];
            sprite_asprite16(rgb, &sprites[i], img, scanline, width);
        }
    }
}

bool mode4_prog(uint16_t *xregs)
{
    const uint16_t attributes = xregs[2];
    const uint16_t config_ptr = xregs[3];
    const int16_t length = xregs[4];
    const int16_t plane = xregs[5];
    const int16_t scanline_begin = xregs[6];
    const int16_t scanline_end = xregs[7];

    if (config_ptr & 1)
        return false;

    void *render_fn;
    switch (attributes)
    {
    case 0:
        render_fn = mode4_render_sprite;
        if (config_ptr > 0x10000 - sizeof(mode4_sprite_t) * length)
            return false;
        break;
    case 1:
        render_fn = mode4_render_asprite;
        if (config_ptr > 0x10000 - sizeof(mode4_asprite_t) * length)
            return false;
        break;
    default:
        return false;
    };

    return vga_prog_sprite(plane, scanline_begin, scanline_end, config_ptr, length, render_fn);
}
