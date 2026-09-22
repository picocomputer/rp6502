/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Whether a palette pointer names a palette in XRAM. A package rather
 * than a copy in each mode because four modes ask the same question and
 * the answer decides whether a plane shows XRAM's colors or the built-in
 * ones, which is not a rule to let drift.
 *
 * The sentinel $FFFF needs no test of its own: a palette is read from
 * XRAM only when it is halfword aligned, and $FFFF is odd.
 */

package pal_range_pkg;

    /* The last entry has to lie below the top of XRAM, and a palette is
     * two bytes an entry, so the limit is $10000 less a power of two.
     * The pointer clears it unless it lands in that last block, which is
     * where the bits above the block are all ones and something inside
     * it is set. */
    function automatic logic pal_fits(input logic [15:0] ptr,
                                      input logic [1:0] bpp_log);
        case (bpp_log)
            2'd0: pal_fits = !(&ptr[15:2] && |ptr[1:0]);
            2'd1: pal_fits = !(&ptr[15:3] && |ptr[2:0]);
            2'd2: pal_fits = !(&ptr[15:5] && |ptr[4:0]);
            default: pal_fits = !(&ptr[15:9] && |ptr[8:0]);
        endcase
    endfunction

endpackage
