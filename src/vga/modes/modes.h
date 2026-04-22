/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_MODES_MODES_H_
#define _VGA_MODES_MODES_H_

// Shared helpers for 1bpp/2bpp scanline renderers

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

static inline __attribute__((always_inline)) void
modes_render_1bpp(uint16_t *buf, uint8_t bits, uint16_t bg, uint16_t fg)
{
    switch (bits >> 4)
    {
    case 0:
        buf[0] = bg;
        buf[1] = bg;
        buf[2] = bg;
        buf[3] = bg;
        break;
    case 1:
        buf[0] = bg;
        buf[1] = bg;
        buf[2] = bg;
        buf[3] = fg;
        break;
    case 2:
        buf[0] = bg;
        buf[1] = bg;
        buf[2] = fg;
        buf[3] = bg;
        break;
    case 3:
        buf[0] = bg;
        buf[1] = bg;
        buf[2] = fg;
        buf[3] = fg;
        break;
    case 4:
        buf[0] = bg;
        buf[1] = fg;
        buf[2] = bg;
        buf[3] = bg;
        break;
    case 5:
        buf[0] = bg;
        buf[1] = fg;
        buf[2] = bg;
        buf[3] = fg;
        break;
    case 6:
        buf[0] = bg;
        buf[1] = fg;
        buf[2] = fg;
        buf[3] = bg;
        break;
    case 7:
        buf[0] = bg;
        buf[1] = fg;
        buf[2] = fg;
        buf[3] = fg;
        break;
    case 8:
        buf[0] = fg;
        buf[1] = bg;
        buf[2] = bg;
        buf[3] = bg;
        break;
    case 9:
        buf[0] = fg;
        buf[1] = bg;
        buf[2] = bg;
        buf[3] = fg;
        break;
    case 10:
        buf[0] = fg;
        buf[1] = bg;
        buf[2] = fg;
        buf[3] = bg;
        break;
    case 11:
        buf[0] = fg;
        buf[1] = bg;
        buf[2] = fg;
        buf[3] = fg;
        break;
    case 12:
        buf[0] = fg;
        buf[1] = fg;
        buf[2] = bg;
        buf[3] = bg;
        break;
    case 13:
        buf[0] = fg;
        buf[1] = fg;
        buf[2] = bg;
        buf[3] = fg;
        break;
    case 14:
        buf[0] = fg;
        buf[1] = fg;
        buf[2] = fg;
        buf[3] = bg;
        break;
    case 15:
        buf[0] = fg;
        buf[1] = fg;
        buf[2] = fg;
        buf[3] = fg;
        break;
    }
    switch (bits & 0xF)
    {
    case 0:
        buf[4] = bg;
        buf[5] = bg;
        buf[6] = bg;
        buf[7] = bg;
        break;
    case 1:
        buf[4] = bg;
        buf[5] = bg;
        buf[6] = bg;
        buf[7] = fg;
        break;
    case 2:
        buf[4] = bg;
        buf[5] = bg;
        buf[6] = fg;
        buf[7] = bg;
        break;
    case 3:
        buf[4] = bg;
        buf[5] = bg;
        buf[6] = fg;
        buf[7] = fg;
        break;
    case 4:
        buf[4] = bg;
        buf[5] = fg;
        buf[6] = bg;
        buf[7] = bg;
        break;
    case 5:
        buf[4] = bg;
        buf[5] = fg;
        buf[6] = bg;
        buf[7] = fg;
        break;
    case 6:
        buf[4] = bg;
        buf[5] = fg;
        buf[6] = fg;
        buf[7] = bg;
        break;
    case 7:
        buf[4] = bg;
        buf[5] = fg;
        buf[6] = fg;
        buf[7] = fg;
        break;
    case 8:
        buf[4] = fg;
        buf[5] = bg;
        buf[6] = bg;
        buf[7] = bg;
        break;
    case 9:
        buf[4] = fg;
        buf[5] = bg;
        buf[6] = bg;
        buf[7] = fg;
        break;
    case 10:
        buf[4] = fg;
        buf[5] = bg;
        buf[6] = fg;
        buf[7] = bg;
        break;
    case 11:
        buf[4] = fg;
        buf[5] = bg;
        buf[6] = fg;
        buf[7] = fg;
        break;
    case 12:
        buf[4] = fg;
        buf[5] = fg;
        buf[6] = bg;
        buf[7] = bg;
        break;
    case 13:
        buf[4] = fg;
        buf[5] = fg;
        buf[6] = bg;
        buf[7] = fg;
        break;
    case 14:
        buf[4] = fg;
        buf[5] = fg;
        buf[6] = fg;
        buf[7] = bg;
        break;
    case 15:
        buf[4] = fg;
        buf[5] = fg;
        buf[6] = fg;
        buf[7] = fg;
        break;
    }
}

static inline __attribute__((always_inline)) void
modes_render_1bpp_reverse(uint16_t *buf, uint8_t bits, uint16_t bg, uint16_t fg)
{
    switch (bits & 0xF)
    {
    case 0:
        buf[3] = bg;
        buf[2] = bg;
        buf[1] = bg;
        buf[0] = bg;
        break;
    case 1:
        buf[3] = bg;
        buf[2] = bg;
        buf[1] = bg;
        buf[0] = fg;
        break;
    case 2:
        buf[3] = bg;
        buf[2] = bg;
        buf[1] = fg;
        buf[0] = bg;
        break;
    case 3:
        buf[3] = bg;
        buf[2] = bg;
        buf[1] = fg;
        buf[0] = fg;
        break;
    case 4:
        buf[3] = bg;
        buf[2] = fg;
        buf[1] = bg;
        buf[0] = bg;
        break;
    case 5:
        buf[3] = bg;
        buf[2] = fg;
        buf[1] = bg;
        buf[0] = fg;
        break;
    case 6:
        buf[3] = bg;
        buf[2] = fg;
        buf[1] = fg;
        buf[0] = bg;
        break;
    case 7:
        buf[3] = bg;
        buf[2] = fg;
        buf[1] = fg;
        buf[0] = fg;
        break;
    case 8:
        buf[3] = fg;
        buf[2] = bg;
        buf[1] = bg;
        buf[0] = bg;
        break;
    case 9:
        buf[3] = fg;
        buf[2] = bg;
        buf[1] = bg;
        buf[0] = fg;
        break;
    case 10:
        buf[3] = fg;
        buf[2] = bg;
        buf[1] = fg;
        buf[0] = bg;
        break;
    case 11:
        buf[3] = fg;
        buf[2] = bg;
        buf[1] = fg;
        buf[0] = fg;
        break;
    case 12:
        buf[3] = fg;
        buf[2] = fg;
        buf[1] = bg;
        buf[0] = bg;
        break;
    case 13:
        buf[3] = fg;
        buf[2] = fg;
        buf[1] = bg;
        buf[0] = fg;
        break;
    case 14:
        buf[3] = fg;
        buf[2] = fg;
        buf[1] = fg;
        buf[0] = bg;
        break;
    case 15:
        buf[3] = fg;
        buf[2] = fg;
        buf[1] = fg;
        buf[0] = fg;
        break;
    }
    switch (bits >> 4)
    {
    case 0:
        buf[7] = bg;
        buf[6] = bg;
        buf[5] = bg;
        buf[4] = bg;
        break;
    case 1:
        buf[7] = bg;
        buf[6] = bg;
        buf[5] = bg;
        buf[4] = fg;
        break;
    case 2:
        buf[7] = bg;
        buf[6] = bg;
        buf[5] = fg;
        buf[4] = bg;
        break;
    case 3:
        buf[7] = bg;
        buf[6] = bg;
        buf[5] = fg;
        buf[4] = fg;
        break;
    case 4:
        buf[7] = bg;
        buf[6] = fg;
        buf[5] = bg;
        buf[4] = bg;
        break;
    case 5:
        buf[7] = bg;
        buf[6] = fg;
        buf[5] = bg;
        buf[4] = fg;
        break;
    case 6:
        buf[7] = bg;
        buf[6] = fg;
        buf[5] = fg;
        buf[4] = bg;
        break;
    case 7:
        buf[7] = bg;
        buf[6] = fg;
        buf[5] = fg;
        buf[4] = fg;
        break;
    case 8:
        buf[7] = fg;
        buf[6] = bg;
        buf[5] = bg;
        buf[4] = bg;
        break;
    case 9:
        buf[7] = fg;
        buf[6] = bg;
        buf[5] = bg;
        buf[4] = fg;
        break;
    case 10:
        buf[7] = fg;
        buf[6] = bg;
        buf[5] = fg;
        buf[4] = bg;
        break;
    case 11:
        buf[7] = fg;
        buf[6] = bg;
        buf[5] = fg;
        buf[4] = fg;
        break;
    case 12:
        buf[7] = fg;
        buf[6] = fg;
        buf[5] = bg;
        buf[4] = bg;
        break;
    case 13:
        buf[7] = fg;
        buf[6] = fg;
        buf[5] = bg;
        buf[4] = fg;
        break;
    case 14:
        buf[7] = fg;
        buf[6] = fg;
        buf[5] = fg;
        buf[4] = bg;
        break;
    case 15:
        buf[7] = fg;
        buf[6] = fg;
        buf[5] = fg;
        buf[4] = fg;
        break;
    }
}

static inline __attribute__((always_inline)) void
modes_emit_head_1bpp(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t start, int16_t count)
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
modes_emit_tail_1bpp(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t fill_cols)
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
modes_emit_head_1bpp_reverse(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t start, int16_t count)
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
modes_emit_tail_1bpp_reverse(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t fill_cols)
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
modes_emit_head_2bpp(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t start, int16_t count)
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
modes_emit_tail_2bpp(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t fill_cols)
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
modes_emit_head_2bpp_reverse(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t start, int16_t count)
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
modes_emit_tail_2bpp_reverse(uint16_t **rgb, uint8_t bits, const uint16_t *pal, int16_t fill_cols)
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

#endif /* _VGA_MODES_MODES_H_ */
