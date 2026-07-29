/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The font store: four faces the terminal and the mode 1 cells read a
 * byte at a time, and the soft CPU owns. font16 and font8 are writable
 * because a code page rewrites their high halves; italic is ASCII only
 * and the DEC graphics never change, so those two ship in the
 * bitstream and stay read-only.
 *
 * Every face is addressed row-major with font.c's own stride, so the
 * hardware image is the C image and the firmware fills the store with
 * the copies font_set_code_page already makes. The writable faces come
 * up blank on purpose: an initial image would render a terminal even
 * with the firmware's stores misaddressed, and every frame test would
 * pass while nothing worked. The read address carries the face in its
 * top two bits:
 *
 *   0  font16    {2'b00, row[3:0], code[7:0]}         4096 B
 *   1  font8     {2'b01, 1'b0, row[2:0], code[7:0]}   2048 B
 *   2  italic16  {2'b10, 1'b0, row[3:0], code[6:0]}   2048 B
 *   3  dec16     {2'b11, 3'b000, row[3:0], idx[4:0]}   512 B
 *
 * Each face is a word wide with byte lanes because that is the shape a
 * block RAM holds cheaply — a byte-wide array of the same depth costs
 * the fabric more blocks than a word-wide one a quarter as deep. All
 * four are read every clock and the face chooses afterward: a face
 * folded into the address ahead of the lookup makes the address a mux
 * across the faces, which is logic rather than memory.
 */

module vid_font
    import vid_font_pkg::*;
(
    input logic clk,

    /* One byte, one clock behind the address. */
    input logic [13:0] addr,
    output logic [7:0] vid_font_bits,

    /* The soft CPU's window, a whole word per write: byte lanes are
     * what stops a dual-port RAM being inferred at all, and the
     * firmware copies fonts in aligned runs anyway. */
    input logic w_stb,
    input logic [13:0] w_addr,
    input logic [31:0] w_data
);

    logic [31:0] f16[1024] /*verilator public_flat_rd*/;
    logic [31:0] f8[512] /*verilator public_flat_rd*/;
    logic [31:0] ital[512] = VID_ITALIC16_W;
    logic [31:0] dec[128] = VID_FONT_DEC16_W;

    logic [1:0] w_face;
    always_comb w_face = w_addr[13:12];

    always_ff @(posedge clk) begin
        if (w_stb && w_face == 2'd0)
            f16[w_addr[11:2]] <= w_data;
        if (w_stb && w_face == 2'd1)
            f8[w_addr[10:2]] <= w_data;
    end

    logic [31:0] q16, q8, q_ital, q_dec;
    logic [1:0] face_q;
    logic [1:0] byte_q;
    always_ff @(posedge clk) begin
        q16 <= f16[addr[11:2]];
        q8 <= f8[addr[10:2]];
        q_ital <= ital[addr[10:2]];
        q_dec <= dec[addr[8:2]];
        face_q <= addr[13:12];
        byte_q <= addr[1:0];
    end

    logic [31:0] word_q;
    always_comb begin
        case (face_q)
            2'd1: word_q = q8;
            2'd2: word_q = q_ital;
            2'd3: word_q = q_dec;
            default: word_q = q16;
        endcase
        vid_font_bits = word_q[{byte_q, 3'b000}+:8];
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_font;
    always_comb unused_vid_font = ^w_addr[1:0];  /* the lanes carry it */
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
