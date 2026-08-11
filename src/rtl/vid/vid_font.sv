/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module vid_font (
    input logic clk,

    input logic [13:0] addr,
    output logic [7:0] vid_font_bits,

    input logic w_stb,
    input logic [13:0] w_addr,
    input logic [31:0] w_data
);

    logic [31:0] f16[1024] ;
    logic [31:0] f8[512] ;
    logic [31:0] ital[512] ;
    (* ramstyle = "no_rw_check" *)
    logic [31:0] dec[256] ;

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
        vid_font_bits = word_q[{byte_q, 3'b000}+:8];
    end

    logic unused_vid_font;
    always_comb unused_vid_font = ^{w_addr[1:0], w_addr_q[1:0]};

endmodule
