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
 *
 * Each face is its own memory, read every clock and chosen afterward. A
 * face selected before the lookup makes the address a mux across three
 * constant tables, and the fabric builds that from logic — near a
 * thousand ALUTs for what block memory holds for nothing.
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

    logic [7:0] rom16[4096] = VID_FONT16;
    logic [7:0] rom_dec[512] = VID_FONT_DEC16;
    logic [7:0] rom_ital[2048] = VID_ITALIC16;

    logic [7:0] q16, q_dec, q_ital;
    logic [1:0] sel_q;
    always_ff @(posedge clk) begin
        q16 <= rom16[{code, row}];
        q_dec <= rom_dec[{code[4:0], row}];
        q_ital <= rom_ital[{code[6:0], row}];
        sel_q <= sel;
    end

    always_comb begin
        case (sel_q)
            2'd1: vid_font_bits = q_dec;
            2'd2: vid_font_bits = q_ital;
            default: vid_font_bits = q16;
        endcase
    end

endmodule
