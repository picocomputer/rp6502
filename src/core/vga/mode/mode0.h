/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_MODE0_H_
#define _CORE_VGA_MODE0_H_

#include "core/vga/prog.h"
#include "core/vga/vga.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool mode0_prog(uint16_t *xregs);

/* The terminal's place on a canvas w by h, which every machine that programs
 * mode 0 works out the same way. BEGIN and END both 0 give the tallest
 * terminal that fits, centered, so a canvas that is not a whole number of
 * rows tall splits the spare scanlines above and below it: 2..178 at 180 and
 * 4..356 at 360. END 0 alone runs to the bottom of the canvas. False when the
 * scanlines between BEGIN and END are not a whole number of rows. */
static inline bool mode0_layout(int16_t w, int16_t h,
                                int16_t *begin, int16_t *end,
                                uint8_t *cols, uint8_t *rows)
{
    const int16_t px = (int16_t)vga_term_row_px(w);
    if (!*begin && !*end)
    {
        *begin = (int16_t)(h % px / 2);
        *end = (int16_t)(*begin + h / px * px);
    }
    if (!*end)
        *end = h;
    const int16_t count = (int16_t)(*end - *begin);
    if (!count || count % px)
        return false;
    *cols = (uint8_t)vga_term_columns(w);
    *rows = (uint8_t)(count / px);
    return true;
}

vga_fill_fn_t mode0_fill_fn(uint16_t attributes);
bool mode0_fill_attr(vga_fill_fn_t fn, uint16_t *attributes);

/* The scanline the terminal's program begins at. It cannot be recovered from
 * the scanline table afterwards, because a graphics mode programmed later can
 * overwrite the lowest-numbered rows the terminal installed. */
int16_t mode0_begin(void);
void mode0_set_begin(int16_t at);

#endif /* _CORE_VGA_MODE0_H_ */
