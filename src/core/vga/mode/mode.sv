/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What the mode engines share, as core/vga/mode/mode.h is what their
 * renderers share.
 */

package mode;

    /* Whether a palette pointer names a palette in XRAM. Four modes ask
     * the same question and the answer decides whether a plane shows
     * XRAM's colors or the built-in ones, which is not a rule to let
     * drift. The sentinel $FFFF needs no test of its own: a palette is
     * read from XRAM only when it is halfword aligned, and $FFFF is odd.
     *
     * The last entry has to lie below the top of XRAM, and a palette is
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

    /* The index of the pixel at bit `at` of a byte, 1, 2, 4 or 8 bits
     * wide, taken from the high bits down unless reversed. */
    function automatic logic [7:0] sub_idx(input logic [7:0] b,
                                           input logic [2:0] at,
                                           input logic [2:0] bpp_log,
                                           input logic rev);
        case (bpp_log)
            3'd0: sub_idx = {7'd0, rev ? b[at] : b[3'd7 - at]};
            3'd1: sub_idx = {6'd0, rev ? b[{at[2:1], 1'b0}+:2]
                                     : b[{2'd3 - at[2:1], 1'b0}+:2]};
            3'd2: sub_idx = {4'd0, rev ? b[{at[2], 2'b00}+:4]
                                     : b[{!at[2], 2'b00}+:4]};
            default: sub_idx = b;
        endcase
    endfunction

endpackage
