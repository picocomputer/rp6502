/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Reaching into the terminal's cell memory from a bench.
 *
 * The cells are four byte-lane arrays so the fabric can hold them in
 * memory at all; a whole cell is those lanes stacked. Shared rather
 * than copied because two benches want it and its shape has already
 * changed once.
 */

#ifndef _TB_TERM_H_
#define _TB_TERM_H_

#include <cstddef>
#include <cstdint>

#define TB_TERM_LANE(r, l, i) (r->wiring__DOT__mode0__DOT__cell##l[i])

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
