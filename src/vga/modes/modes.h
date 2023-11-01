/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MODES_H_
#define _MODES_H_

// Common utils for all modes

#include <stdint.h>
#include <stdbool.h>

static inline __attribute__((always_inline)) void __attribute__((optimize("O1")))
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

static inline __attribute__((always_inline)) void __attribute__((optimize("O1")))
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

#endif /* _MODES_H_ */
