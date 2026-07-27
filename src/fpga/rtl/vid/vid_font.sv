/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The terminal font ROM: the generated tables from vga/term/font.c, one
 * synchronous lookup per cell fetch. The face select mirrors the renderer:
 * DEC Special Graphics swaps the glyph source for codes 0x5F-0x7E, italic
 * has the low half only, everything else is font16 — ASCII low, CP437
 * high, the fixed boot code page until the font store becomes writable.
 */

module vid_font
    import vid_font_pkg::*;
(
    input logic clk,

    /* 0 font16, 1 DEC (code already rebased to 0..0x1F), 2 italic. */
    input logic [1:0] sel,
    input logic [7:0] code,
    input logic [3:0] row,
    output logic [7:0] vid_font_bits
);

    always_ff @(posedge clk) begin
        case (sel)
            2'd1: vid_font_bits <= VID_FONT_DEC16[{code[4:0], row}];
            2'd2: vid_font_bits <= VID_ITALIC16[{code[6:0], row}];
            default: vid_font_bits <= VID_FONT16[{code, row}];
        endcase
    end

endmodule
