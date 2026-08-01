/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Reaching into the terminal's cell memory from a bench.
 *
 * The cells are four byte-lane arrays so the fabric can hold them in
 * memory at all, and each lane is banked 8192 + 4096 + 2048 + 1024 so
 * Quartus stops rounding 15360 up to 16384 and charging a block a lane
 * for the difference. A whole cell is the four lanes of one bank,
 * stacked. Shared rather than copied because two benches wanted it and
 * the banking changed both.
 */

#ifndef _TB_TERM_H_
#define _TB_TERM_H_

#include <cstddef>
#include <cstdint>

#define TB_TERM_LANE(r, l, i)                                                  \
    (*((i) < 8192      ? &r->rp6502__DOT__vid_term__DOT__cell##l##_b0[i]       \
       : (i) < 12288   ? &r->rp6502__DOT__vid_term__DOT__cell##l##_b1[(i) - 8192]  \
       : (i) < 14336   ? &r->rp6502__DOT__vid_term__DOT__cell##l##_b2[(i) - 12288] \
                       : &r->rp6502__DOT__vid_term__DOT__cell##l##_b3[(i) - 14336]))

template <typename Root>
static uint32_t term_cell(Root *r, size_t i)
{
    return (uint32_t)TB_TERM_LANE(r, 0, i)
        | ((uint32_t)TB_TERM_LANE(r, 1, i) << 8)
        | ((uint32_t)TB_TERM_LANE(r, 2, i) << 16)
        | ((uint32_t)TB_TERM_LANE(r, 3, i) << 24);
}

template <typename Root>
static void term_cell_set(Root *r, size_t i, uint32_t v)
{
    TB_TERM_LANE(r, 0, i) = (uint8_t)v;
    TB_TERM_LANE(r, 1, i) = (uint8_t)(v >> 8);
    TB_TERM_LANE(r, 2, i) = (uint8_t)(v >> 16);
    TB_TERM_LANE(r, 3, i) = (uint8_t)(v >> 24);
}

#endif /* _TB_TERM_H_ */
