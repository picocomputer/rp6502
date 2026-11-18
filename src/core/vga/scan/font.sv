/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The font store, two fonts read a byte at a time. Nothing is built into
 * the bitstream: the store comes up blank and the firmware fills it from
 * the tables core/term/font.c builds.
 *
 * Each font is core/term/font.h's: two tables of 256 glyphs, each
 * row-major, so the hardware image is the C image. The first table is
 * the code page's, which is all mode 1 draws. The second has the italics
 * and DEC Special Graphics, which mode 0's glyphs reach. The read address
 * carries the font in its top bit:
 *
 *   font16  {1'b0, table, row[3:0], code[7:0]}         8192 B
 *   font8   {2'b10, table, row[2:0], code[7:0]}        4096 B
 *
 * Each font is a word wide with byte lanes because that is the shape a
 * block RAM holds cheaply.
 */

module font (
    input logic clk,

    input logic [13:0] addr,
    output logic [7:0] font_bits,

    /* The soft CPU's window, a whole word per write: byte lanes would
     * stop a dual-port RAM being inferred at all, and the firmware
     * copies fonts in aligned runs anyway. */
    input logic w_stb,
    input logic [13:0] w_addr,
    input logic [31:0] w_data
);

    logic [31:0] f16[2048] /*verilator public_flat_rd*/;
    logic [31:0] f8[1024] /*verilator public_flat_rd*/;

    always_ff @(posedge clk) begin
        if (w_stb && !w_addr[13])
            f16[w_addr[12:2]] <= w_data;
        if (w_stb && w_addr[13])
            f8[w_addr[11:2]] <= w_data;
    end

    logic [31:0] q16, q8;
    logic face_q;
    logic [1:0] byte_q;
    always_ff @(posedge clk) begin
        q16 <= f16[addr[12:2]];
        q8 <= f8[addr[11:2]];
        face_q <= addr[13];
        byte_q <= addr[1:0];
    end

    logic [31:0] word_q;
    always_comb begin
        word_q = face_q ? q8 : q16;
        font_bits = word_q[{byte_q, 3'b000}+:8];
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_font;
    /* Writes are whole words, so the byte within the word selects
     * nothing, and font8 is half of its half of the window. */
    always_comb unused_font = ^{w_addr[1:0], w_addr[12], addr[12]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
