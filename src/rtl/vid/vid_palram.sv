/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module vid_palram
    import vid_palette_pkg::*;
(
    input logic clk,

    input logic ld,
    input logic [7:0] w,
    input logic [8:0] words,
    input logic half,
    input logic [31:0] a_rdata,

    input logic xram,
    input logic one_bpp,
    input logic [7:0] idx_a,
    input logic [7:0] idx_b,
    output logic [15:0] vid_palram_qa,
    output logic [15:0] vid_palram_qb
);

    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_a_even[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_a_odd[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_b_even[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_b_odd[128];

    logic we_e, we_o;
    logic [6:0] wa_o;
    logic [15:0] wd_e, wd_o;
    always_comb begin
        we_e = ld && (!half || {1'b0, w} != words);
        we_o = ld && (!half || w != 8'd0);
        wa_o = half ? 7'(w - 8'd1) : w[6:0];
        wd_e = half ? a_rdata[31:16] : a_rdata[15:0];
        wd_o = half ? a_rdata[15:0] : a_rdata[31:16];
    end

    always_ff @(posedge clk) begin
        if (we_e) begin
            pal_a_even[w[6:0]] <= wd_e;
            pal_b_even[w[6:0]] <= wd_e;
        end
        if (we_o) begin
            pal_a_odd[wa_o] <= wd_o;
            pal_b_odd[wa_o] <= wd_o;
        end
    end

    always_comb begin
        if (xram) begin
            vid_palram_qa = idx_a[0] ? pal_a_odd[idx_a[7:1]]
                                     : pal_a_even[idx_a[7:1]];
            vid_palram_qb = idx_b[0] ? pal_b_odd[idx_b[7:1]]
                                     : pal_b_even[idx_b[7:1]];
        end else if (one_bpp) begin
            vid_palram_qa = VID_COLOR_2[idx_a[0]];
            vid_palram_qb = VID_COLOR_2[idx_b[0]];
        end else begin
            vid_palram_qa = VID_COLOR_256[idx_a];
            vid_palram_qb = VID_COLOR_256[idx_b];
        end
    end

endmodule
