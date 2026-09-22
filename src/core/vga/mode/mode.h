/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_MODE_H_
#define _CORE_VGA_MODE_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* GCC folds the index scaling into the load, ldr rN,[rBase,rIdx,lsl #2], only
 * while the table base is a register it holds. Left an sp-relative local it
 * spends an extra add per pixel pair, four more instructions on every cell of
 * every scanline. Nothing is emitted for the asm; it only stops the base being
 * rematerialized from sp. */
#if defined(__GNUC__) && defined(__arm__)
#define MODE_PIN(name) __asm__("" : "+r"(name))
#else
#define MODE_PIN(name) ((void)0)
#endif

#define MODE_TABLE(name, n)          \
    uint32_t name##_storage[n];      \
    uint32_t *name = name##_storage; \
    MODE_PIN(name)

/* A scanline buffer word is two pixels, the left one in the low half. Expanding
 * through a table of the four ways a pixel pair can land costs no branches,
 * where a switch on a nibble costs an indirect one per nibble. */
typedef uint32_t mode_word_t __attribute__((aligned(1), may_alias));

/* MSVC defines neither macro. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "a pixel pair packs the left pixel in the low half");
#endif

static inline __attribute__((always_inline)) uint32_t
mode_pack2(uint16_t left, uint16_t right)
{
    return (uint32_t)left | ((uint32_t)right << 16);
}

/* Indexed by two glyph bits, the left pixel's bit the higher of the two. */
static inline __attribute__((always_inline)) void
mode_pair_set(uint32_t *pair, uint16_t bg, uint16_t fg)
{
    pair[0] = mode_pack2(bg, bg);
    pair[1] = mode_pack2(bg, fg);
    pair[2] = mode_pack2(fg, bg);
    pair[3] = mode_pack2(fg, fg);
}

/* Reversing the bit order within the byte only swaps the mixed entries. */
static inline __attribute__((always_inline)) void
mode_pair_set_reverse(uint32_t *pair, uint16_t bg, uint16_t fg)
{
    pair[0] = mode_pack2(bg, bg);
    pair[1] = mode_pack2(fg, bg);
    pair[2] = mode_pack2(bg, fg);
    pair[3] = mode_pack2(fg, fg);
}

static inline __attribute__((always_inline)) void
mode_render_1bpp(uint16_t *buf, uint8_t bits, const uint32_t *pair)
{
    mode_word_t *w = (mode_word_t *)buf;
    w[0] = pair[bits >> 6];
    w[1] = pair[(bits >> 4) & 3];
    w[2] = pair[(bits >> 2) & 3];
    w[3] = pair[bits & 3];
}

static inline __attribute__((always_inline)) void
mode_render_1bpp_reverse(uint16_t *buf, uint8_t bits, const uint32_t *pair)
{
    mode_word_t *w = (mode_word_t *)buf;
    w[0] = pair[bits & 3];
    w[1] = pair[(bits >> 2) & 3];
    w[2] = pair[(bits >> 4) & 3];
    w[3] = pair[bits >> 6];
}

/* A 2bpp nibble is a whole pixel pair, so one byte is two words. */
static inline __attribute__((always_inline)) void
mode_quad_set(uint32_t *quad, const uint16_t *pal)
{
    for (int i = 0; i < 16; i++)
        quad[i] = mode_pack2(pal[i >> 2], pal[i & 3]);
}

static inline __attribute__((always_inline)) void
mode_quad_set_reverse(uint32_t *quad, const uint16_t *pal)
{
    for (int i = 0; i < 16; i++)
        quad[i] = mode_pack2(pal[i & 3], pal[i >> 2]);
}

static inline __attribute__((always_inline)) void
mode_render_2bpp(uint16_t *buf, uint8_t bits, const uint32_t *quad)
{
    mode_word_t *w = (mode_word_t *)buf;
    w[0] = quad[bits >> 4];
    w[1] = quad[bits & 0x0F];
}

static inline __attribute__((always_inline)) void
mode_render_2bpp_reverse(uint16_t *buf, uint8_t bits, const uint32_t *quad)
{
    mode_word_t *w = (mode_word_t *)buf;
    w[0] = quad[bits & 0x0F];
    w[1] = quad[bits >> 4];
}

static inline __attribute__((always_inline)) void
mode_emit_head_1bpp(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t start, int16_t count)
{
    bits >>= 8 - start - count;
    switch (count)
    {
    case 8:
        *(*rgb)++ = pal[(bits & 0x80) >> 7];
        __attribute__((fallthrough));
    case 7:
        *(*rgb)++ = pal[(bits & 0x40) >> 6];
        __attribute__((fallthrough));
    case 6:
        *(*rgb)++ = pal[(bits & 0x20) >> 5];
        __attribute__((fallthrough));
    case 5:
        *(*rgb)++ = pal[(bits & 0x10) >> 4];
        __attribute__((fallthrough));
    case 4:
        *(*rgb)++ = pal[(bits & 0x08) >> 3];
        __attribute__((fallthrough));
    case 3:
        *(*rgb)++ = pal[(bits & 0x04) >> 2];
        __attribute__((fallthrough));
    case 2:
        *(*rgb)++ = pal[(bits & 0x02) >> 1];
        __attribute__((fallthrough));
    case 1:
        *(*rgb)++ = pal[bits & 0x01];
    }
}

static inline __attribute__((always_inline)) void
mode_emit_tail_1bpp(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t fill_cols)
{
    bits >>= 8 - fill_cols;
    switch (fill_cols)
    {
    case 7:
        *(*rgb)++ = pal[(bits & 0x40) >> 6];
        __attribute__((fallthrough));
    case 6:
        *(*rgb)++ = pal[(bits & 0x20) >> 5];
        __attribute__((fallthrough));
    case 5:
        *(*rgb)++ = pal[(bits & 0x10) >> 4];
        __attribute__((fallthrough));
    case 4:
        *(*rgb)++ = pal[(bits & 0x08) >> 3];
        __attribute__((fallthrough));
    case 3:
        *(*rgb)++ = pal[(bits & 0x04) >> 2];
        __attribute__((fallthrough));
    case 2:
        *(*rgb)++ = pal[(bits & 0x02) >> 1];
        __attribute__((fallthrough));
    case 1:
        *(*rgb)++ = pal[bits & 0x01];
    }
}

static inline __attribute__((always_inline)) void
mode_emit_head_1bpp_reverse(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t start, int16_t count)
{
    bits <<= 8 - start - count;
    switch (count)
    {
    case 8:
        *(*rgb)++ = pal[bits & 0x01];
        __attribute__((fallthrough));
    case 7:
        *(*rgb)++ = pal[(bits & 0x02) >> 1];
        __attribute__((fallthrough));
    case 6:
        *(*rgb)++ = pal[(bits & 0x04) >> 2];
        __attribute__((fallthrough));
    case 5:
        *(*rgb)++ = pal[(bits & 0x08) >> 3];
        __attribute__((fallthrough));
    case 4:
        *(*rgb)++ = pal[(bits & 0x10) >> 4];
        __attribute__((fallthrough));
    case 3:
        *(*rgb)++ = pal[(bits & 0x20) >> 5];
        __attribute__((fallthrough));
    case 2:
        *(*rgb)++ = pal[(bits & 0x40) >> 6];
        __attribute__((fallthrough));
    case 1:
        *(*rgb)++ = pal[(bits & 0x80) >> 7];
    }
}

static inline __attribute__((always_inline)) void
mode_emit_tail_1bpp_reverse(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t fill_cols)
{
    bits <<= 8 - fill_cols;
    switch (fill_cols)
    {
    case 7:
        *(*rgb)++ = pal[(bits & 0x02) >> 1];
        __attribute__((fallthrough));
    case 6:
        *(*rgb)++ = pal[(bits & 0x04) >> 2];
        __attribute__((fallthrough));
    case 5:
        *(*rgb)++ = pal[(bits & 0x08) >> 3];
        __attribute__((fallthrough));
    case 4:
        *(*rgb)++ = pal[(bits & 0x10) >> 4];
        __attribute__((fallthrough));
    case 3:
        *(*rgb)++ = pal[(bits & 0x20) >> 5];
        __attribute__((fallthrough));
    case 2:
        *(*rgb)++ = pal[(bits & 0x40) >> 6];
        __attribute__((fallthrough));
    case 1:
        *(*rgb)++ = pal[(bits & 0x80) >> 7];
    }
}

static inline __attribute__((always_inline)) void
mode_emit_head_2bpp(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t start, int16_t count)
{
    bits >>= 2 * (4 - start - count);
    switch (count)
    {
    case 4:
        *(*rgb)++ = pal[(bits & 0xC0) >> 6];
        __attribute__((fallthrough));
    case 3:
        *(*rgb)++ = pal[(bits & 0x30) >> 4];
        __attribute__((fallthrough));
    case 2:
        *(*rgb)++ = pal[(bits & 0x0C) >> 2];
        __attribute__((fallthrough));
    case 1:
        *(*rgb)++ = pal[bits & 0x03];
    }
}

static inline __attribute__((always_inline)) void
mode_emit_tail_2bpp(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t fill_cols)
{
    bits >>= 2 * (4 - fill_cols);
    switch (fill_cols)
    {
    case 3:
        *(*rgb)++ = pal[(bits & 0x30) >> 4];
        __attribute__((fallthrough));
    case 2:
        *(*rgb)++ = pal[(bits & 0x0C) >> 2];
        __attribute__((fallthrough));
    case 1:
        *(*rgb)++ = pal[bits & 0x03];
    }
}

static inline __attribute__((always_inline)) void
mode_emit_head_2bpp_reverse(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t start, int16_t count)
{
    bits <<= 2 * (4 - start - count);
    switch (count)
    {
    case 4:
        *(*rgb)++ = pal[bits & 0x03];
        __attribute__((fallthrough));
    case 3:
        *(*rgb)++ = pal[(bits & 0x0C) >> 2];
        __attribute__((fallthrough));
    case 2:
        *(*rgb)++ = pal[(bits & 0x30) >> 4];
        __attribute__((fallthrough));
    case 1:
        *(*rgb)++ = pal[(bits & 0xC0) >> 6];
    }
}

static inline __attribute__((always_inline)) void
mode_emit_tail_2bpp_reverse(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t fill_cols)
{
    bits <<= 2 * (4 - fill_cols);
    switch (fill_cols)
    {
    case 3:
        *(*rgb)++ = pal[(bits & 0x0C) >> 2];
        __attribute__((fallthrough));
    case 2:
        *(*rgb)++ = pal[(bits & 0x30) >> 4];
        __attribute__((fallthrough));
    case 1:
        *(*rgb)++ = pal[(bits & 0xC0) >> 6];
    }
}

#endif /* _CORE_VGA_MODE_H_ */
