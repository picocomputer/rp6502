/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module vid_prog (
    input logic clk,
    input logic clk_mem,
    input logic frame_start,

    input logic [9:0] v,
    input logic px_first,
    output logic vid_prog_vsync_pulse,
    input logic [9:0] h,

    output logic [2:0] vid_prog_canvas,
    output logic [9:0] vid_prog_cw,
    output logic [9:0] vid_prog_ch,

    input logic [8:0] p_line,
    input logic [1:0] p_plane,
    output logic [31:0] vid_prog_p_entry,
    output logic [15:0] vid_prog_p_config,

    input logic [12:0] s_idx,
    output logic [31:0] vid_prog_s_data,

    input logic sst_own,
    input logic [10:0] sst_addr,
    input logic [1:0] sst_word,
    input logic sst_we,
    input logic [31:0] sst_wdata,
    output logic [31:0] vid_prog_sst_rdata,

    input logic b_stb,
    input logic b_we,
    input logic [15:0] b_addr,
    input logic [31:0] b_wdata,
    output logic [31:0] vid_prog_b_rdata
);

    (* ramstyle = "no_rw_check" *)
    logic [31:0] fill_e[2048] ;
    (* ramstyle = "no_rw_check" *)
    logic [15:0] fill_c[2048] ;
    (* ramstyle = "no_rw_check" *)
    logic [19:0] spr_e[2048] ;
    (* ramstyle = "no_rw_check" *)
    logic [31:0] spr_c[2048] ;

    logic [10:0] b_idx;
    always_comb b_idx = b_addr[14:4];

    logic [10:0] p_a, s_a;
    always_comb begin
        p_a = sst_own ? sst_addr : {p_line, p_plane};
        s_a = sst_own ? sst_addr : s_idx[12:2];
    end

    logic [2:0] canvas_shadow ;
    logic [9:0] vsync_shadow ;
    logic [9:0] vsync_q;

    logic b_tbl_q;
    logic [1:0] b_word_q;
    logic [31:0] b_reg_q;
    initial begin
        b_tbl_q = 1'b0;
        b_word_q = 2'd0;
        b_reg_q = 32'd0;
    end
    logic [10:0] w_a;
    logic [1:0] w_word;
    logic [31:0] w_d;
    logic w_go;
    always_comb begin
        w_a = sst_own ? sst_addr : b_idx;
        w_word = sst_own ? sst_word : b_addr[3:2];
        w_d = sst_own ? sst_wdata : b_wdata;
        w_go = sst_own ? sst_we : (b_stb && !b_addr[15] && b_we);
    end
    always_ff @(posedge clk_mem) begin
        if (w_go) begin
            if (w_word == 2'd0) fill_e[w_a] <= w_d;
            if (w_word == 2'd1) fill_c[w_a] <= w_d[15:0];
            if (w_word == 2'd2) spr_e[w_a] <= {w_d[31], w_d[18:0]};
            if (w_word == 2'd3) spr_c[w_a] <= w_d;
        end
        if (b_stb && !sst_own) begin
            b_tbl_q <= !b_addr[15];
            b_word_q <= b_addr[3:2];
            b_reg_q <= !b_addr[15] || b_addr[3] ? 32'd0
                : (b_addr[2] ? {22'd0, vsync_shadow}
                             : {29'd0, canvas_shadow});
        end
    end

    logic [19:0] s_e_q;
    logic [31:0] s_c_q;
    logic s_half_q;

    logic [31:0] b_tbl;
    always_comb begin
        case (b_word_q)
            2'd0: b_tbl = vid_prog_p_entry;
            2'd1: b_tbl = {16'd0, vid_prog_p_config};
            2'd2: b_tbl = {s_e_q[19], 12'd0, s_e_q[18:0]};
            default: b_tbl = s_c_q;
        endcase
        vid_prog_b_rdata = b_tbl_q ? b_tbl : b_reg_q;
        case (sst_word)
            2'd0: vid_prog_sst_rdata = vid_prog_p_entry;
            2'd1: vid_prog_sst_rdata = {16'd0, vid_prog_p_config};
            2'd2: vid_prog_sst_rdata = {s_e_q[19], 12'd0, s_e_q[18:0]};
            default: vid_prog_sst_rdata = s_c_q;
        endcase
    end

    initial begin
        canvas_shadow = 3'd0;
        vsync_shadow = 10'd480;
        vsync_q = 10'd480;
        vid_prog_canvas = 3'd0;
    end
    always_ff @(posedge clk) begin
        if (b_stb && b_we && b_addr[15] && !b_addr[3]) begin
            if (b_addr[2])
                vsync_shadow <= b_wdata[9:0];
            else
                canvas_shadow <= b_wdata[2:0];
        end
        if (h == 10'd0 && px_first && v == 10'd524)
            vid_prog_canvas <= canvas_shadow;
        if (frame_start)
            vsync_q <= vsync_shadow;
    end

    always_comb begin
        vid_prog_cw = (vid_prog_canvas == 3'd1 || vid_prog_canvas == 3'd2)
            ? 10'd320 : 10'd640;
        vid_prog_ch = vid_prog_canvas == 3'd1 ? 10'd240
            : vid_prog_canvas == 3'd2 ? 10'd180
            : vid_prog_canvas == 3'd4 ? 10'd360 : 10'd480;
        vid_prog_vsync_pulse = h == 10'd0 && px_first && v == vsync_q;
    end

    always_ff @(posedge clk_mem) begin
        vid_prog_p_entry <= fill_e[p_a];
        vid_prog_p_config <= fill_c[p_a];
        s_e_q <= spr_e[s_a];
        s_c_q <= spr_c[s_a];
        s_half_q <= s_idx[0];
    end
    always_comb vid_prog_s_data =
        s_half_q ? s_c_q : {s_e_q[19], 12'd0, s_e_q[18:0]};

    logic unused_vid_prog;
    always_comb unused_vid_prog = ^{b_addr[1:0], b_wdata[31:16], s_idx[1]};

endmodule
