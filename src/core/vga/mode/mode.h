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
    const uint32_t c[4] = {pal[0], pal[1], pal[2], pal[3]};
#pragma GCC unroll 16
    for (int i = 0; i < 16; i++)
        quad[i] = c[i >> 2] | (c[i & 3] << 16);
}

static inline __attribute__((always_inline)) void
mode_quad_set_reverse(uint32_t *quad, const uint16_t *pal)
{
    const uint32_t c[4] = {pal[0], pal[1], pal[2], pal[3]};
#pragma GCC unroll 16
    for (int i = 0; i < 16; i++)
        quad[i] = c[i & 3] | (c[i >> 2] << 16);
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

/* Runs of packed pixels for the bitmap modes. The source is read a word at a
 * time where it can be, so the loads that feed the palette are few and
 * independent. Stores are a word of two pixels each. The leftmost pixel is in
 * the most significant bits of a byte unless reversed. */

static inline __attribute__((always_inline)) uint32_t
mode_load32(const uint8_t *src)
{
    return *(const mode_word_t *)src;
}

/* Two pixels of src2 over two of dst2: each half of src2 whose alpha bit is
 * set replaces that half of dst2. With the DSP extension, the alpha bits
 * shifted to the tops of the halves set the GE flags through UADD16, and SEL
 * picks the halves. */
#if defined(__ARM_FEATURE_SIMD32) && __ARM_FEATURE_SIMD32
#include <arm_acle.h>
#endif
static inline __attribute__((always_inline)) uint32_t
mode_alpha_merge(uint32_t src2, uint32_t dst2)
{
#if defined(__ARM_FEATURE_SIMD32) && __ARM_FEATURE_SIMD32
    const uint32_t t = src2 << 10;
    (void)__uadd16(t, t);
    return __sel(src2, dst2);
#else
    const uint32_t m = ((src2 >> 5) & 0x00010001u) * 0xFFFFu;
    return (dst2 & ~m) | (src2 & m);
#endif
}

/* n pixels of 8bpp. dst is set word aligned first, so the stores can pair
 * up, which here is worth 5%. */
static inline __attribute__((always_inline)) void
mode_run_8bpp(uint16_t *dst, const uint8_t *src, unsigned n, const uint16_t *pal)
{
    if (n && ((uintptr_t)dst & 2))
    {
        *dst++ = pal[*src++];
        n--;
    }
    uint32_t *d = (uint32_t *)dst;
    for (; n >= 4; n -= 4, src += 4, d += 2)
    {
        const uint32_t s = mode_load32(src);
        const uint32_t a = mode_pack2(pal[s & 0xFF], pal[(s >> 8) & 0xFF]);
        const uint32_t b = mode_pack2(pal[(s >> 16) & 0xFF], pal[s >> 24]);
        d[0] = a;
        d[1] = b;
    }
    dst = (uint16_t *)d;
    for (; n; n--)
        *dst++ = pal[*src++];
}

/* n pixels of 4bpp, starting at the high nibble of src[0], or the low one
 * when reversed. dst is set word aligned first, as at 8bpp. */
static inline __attribute__((always_inline)) void
mode_run_4bpp(uint16_t *dst, const uint8_t *src, unsigned n, const uint16_t *pal, bool reverse)
{
    const unsigned l = reverse ? 0 : 4, r = reverse ? 4 : 0;
    if (n >= 2 && ((uintptr_t)dst & 2))
    {
        /* One pixel would set dst on a word but leave the source part way
         * through a byte, so this spends the whole byte, and a run that
         * starts off a word stays off it. */
        const uint8_t b = *src++;
        dst[0] = pal[(b >> l) & 0xF];
        dst[1] = pal[(b >> r) & 0xF];
        dst += 2;
        n -= 2;
    }
    if (!((uintptr_t)dst & 2))
    {
        uint32_t *d = (uint32_t *)dst;
        for (; n >= 8; n -= 8, src += 4, d += 4)
        {
            const uint32_t s = mode_load32(src);
            const uint32_t w0 = mode_pack2(pal[(s >> l) & 0xF], pal[(s >> r) & 0xF]);
            const uint32_t w1 = mode_pack2(pal[(s >> (8 + l)) & 0xF], pal[(s >> (8 + r)) & 0xF]);
            const uint32_t w2 = mode_pack2(pal[(s >> (16 + l)) & 0xF], pal[(s >> (16 + r)) & 0xF]);
            const uint32_t w3 = mode_pack2(pal[(s >> (24 + l)) & 0xF], pal[(s >> (24 + r)) & 0xF]);
            d[0] = w0;
            d[1] = w1;
            d[2] = w2;
            d[3] = w3;
        }
        dst = (uint16_t *)d;
    }
    for (; n >= 2; n -= 2, dst += 2)
    {
        const uint8_t b = *src++;
        *(mode_word_t *)dst = mode_pack2(pal[(b >> l) & 0xF], pal[(b >> r) & 0xF]);
    }
    if (n)
        *dst = pal[(*src >> l) & 0xF];
}

/* nbytes bytes of 2bpp through a mode_quad_set table. */
static inline __attribute__((always_inline)) void
mode_run_2bpp(uint16_t *dst, const uint8_t *src, unsigned nbytes, const uint32_t *quad, bool reverse)
{
    for (; nbytes; nbytes--, dst += 4)
        if (reverse)
            mode_render_2bpp_reverse(dst, *src++, quad);
        else
            mode_render_2bpp(dst, *src++, quad);
}

/* Indexed by a nibble of 1bpp pixels, leftmost in its high bit: the four
 * pixels as two words. Built from the four pixel pairs held in registers,
 * fully unrolled, because a loop that indexes the colors costs ten times as
 * much. */
static inline __attribute__((always_inline)) void
mode_nibble_set(uint32_t *tbl, uint16_t bg, uint16_t fg)
{
    const uint32_t p[4] = {mode_pack2(bg, bg), mode_pack2(bg, fg),
                           mode_pack2(fg, bg), mode_pack2(fg, fg)};
#pragma GCC unroll 16
    for (int i = 0; i < 16; i++)
    {
        tbl[i * 2 + 0] = p[i >> 2];
        tbl[i * 2 + 1] = p[i & 3];
    }
}

/* nbytes bytes of 1bpp through a mode_pair_set table, or when reversed a
 * mode_pair_set_reverse table. */
static inline __attribute__((always_inline)) void
mode_run_1bpp(uint16_t *dst, const uint8_t *src, unsigned nbytes, const uint32_t *pair, bool reverse)
{
    for (; nbytes; nbytes--, dst += 8)
        if (reverse)
            mode_render_1bpp_reverse(dst, *src++, pair);
        else
            mode_render_1bpp(dst, *src++, pair);
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
