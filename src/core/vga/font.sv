/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The font store: four faces read a byte at a time. Nothing is built
 * into the bitstream — the store comes up blank and the firmware fills
 * it from the font asset, which is what lets seventeen code pages exist
 * without seventeen code pages of code memory. Blank is also what makes
 * a misaddressed store visible: an initial image would render a
 * terminal anyway and every frame test would pass.
 *
 * Faces are row-major with the C's own stride, so the hardware image is
 * the C image. The read address carries the face in its top two bits:
 *
 *   0  font16    {2'b00, row[3:0], code[7:0]}         4096 B
 *   1  font8     {2'b01, 1'b0, row[2:0], code[7:0]}   2048 B
 *   2  italic16  {2'b10, 1'b0, row[3:0], code[6:0]}   2048 B
 *   3  dec16     {2'b11, 3'b000, row[3:0], idx[4:0]}   512 B
 *      dec8      {2'b11, 3'b001, 1'b0, row[2:0], idx[4:0]} above it
 *
 * Each face is a word wide with byte lanes because that is the shape a
 * block RAM holds cheaply — a byte-wide array of the same depth costs
 * the fabric more blocks than a word-wide one a quarter as deep. All
 * four are read every clock and the face chooses afterward: a face
 * folded into the address ahead of the lookup makes the address a mux
 * across the faces, which is logic rather than memory.
 */

module font (
    input logic clk,

    input logic [13:0] addr,
    output logic [7:0] font_bits,

    /* The soft CPU's window, a whole word per write: byte lanes are
     * what stops a dual-port RAM being inferred at all, and the
     * firmware copies fonts in aligned runs anyway. */
    input logic w_stb,
    input logic [13:0] w_addr,
    input logic [31:0] w_data
);

    logic [31:0] f16[1024] /*verilator public_flat_rd*/;
    logic [31:0] f8[512] /*verilator public_flat_rd*/;
    logic [31:0] ital[512] /*verilator public_flat_rd*/;
    /* The smallest face by a long way, and nothing reads it while
     * writing it. */
    (* ramstyle = "no_rw_check" *)
    logic [31:0] dec[256] /*verilator public_flat_rd*/;

    /* A clock of its own, for hold rather than setup: the soft CPU's
     * address reaches these arrays through nothing but wiring, so at the
     * fast corner the data can arrive before the launching edge. Padding
     * a route that short is something the fitter must rediscover every
     * placement; a register ends it.
     *
     * Free here — nothing reads a face until the firmware has finished
     * writing it. */
    logic w_stb_q;
    logic [13:0] w_addr_q;
    logic [31:0] w_data_q;
    initial begin
        w_stb_q = 1'b0;
        w_addr_q = '0;
        w_data_q = '0;
    end
    always_ff @(posedge clk) begin
        w_stb_q <= w_stb;
        w_addr_q <= w_addr;
        w_data_q <= w_data;
    end

    logic [1:0] w_face;
    always_comb w_face = w_addr_q[13:12];

    always_ff @(posedge clk) begin
        if (w_stb_q && w_face == 2'd0)
            f16[w_addr_q[11:2]] <= w_data_q;
        if (w_stb_q && w_face == 2'd1)
            f8[w_addr_q[10:2]] <= w_data_q;
        if (w_stb_q && w_face == 2'd2)
            ital[w_addr_q[10:2]] <= w_data_q;
        if (w_stb_q && w_face == 2'd3)
            dec[w_addr_q[9:2]] <= w_data_q;
    end

    logic [31:0] q16, q8, q_ital, q_dec;
    logic [1:0] face_q;
    logic [1:0] byte_q;
    always_ff @(posedge clk) begin
        q16 <= f16[addr[11:2]];
        q8 <= f8[addr[10:2]];
        q_ital <= ital[addr[10:2]];
        q_dec <= dec[addr[9:2]];
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
        font_bits = word_q[{byte_q, 3'b000}+:8];
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_font;
    always_comb unused_font = ^{w_addr[1:0], w_addr_q[1:0]};  /* the lanes carry it */
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
