/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One plane's line engine: the scaffolding every mode shares, working
 * one raster line ahead of the beam into a ping-pong line buffer. The
 * subengine owns the XRAM channel and the pixel port until it reports
 * done and whether the plane counts as filled.
 *
 * On line-doubled canvases a row renders once, on the raster line where
 * it first appears, and the buffer holds through the repeat — never a
 * re-render the oracle could not have seen.
 */

module vid_mode (
    input logic clk,
    input logic rst_n,

    input logic [9:0] v,
    input logic [9:0] h,
    input logic px_last,
    input logic line_start,

    /* On the console canvas the planes idle, which is the oracle's fill
     * guard and also shields the unreset prog BRAM across a warm
     * reset. */
    input logic console,
    input logic x_shift,
    input logic y_shift,
    input logic [9:0] y_offset,

    output logic [8:0] vid_mode_p_line,  /* comb: valid whenever granted */
    input logic p_gnt,
    input logic [31:0] p_entry,
    input logic [15:0] p_config,

    /* gnt means the address was taken; the word arrives next clock. */
    output logic vid_mode_a_req,
    output logic [13:0] vid_mode_a_addr,
    input logic a_gnt,

    output logic vid_mode_f_req,
    output logic [13:0] vid_mode_f_addr,
    input logic f_gnt,
    input logic [7:0] f_data,
    input logic [31:0] a_rdata,

    output logic [15:0] vid_mode_pix,
    output logic vid_mode_filled
);

    /* The bank rides inside the address and the output register carries
     * only the buffer: either one broken keeps the line out of block
     * memory. */
    (* ramstyle = "no_rw_check" *)
    logic [15:0] linebuf[2048];
    logic wr_bank;
    logic filled_q[2] /*verilator public_flat_rd*/;
    logic flip_next;

    /* The beam reads one ahead of itself, on each pixel's last tick, and
     * the bank flip lands on h==0's first tick — so only the pixel-0
     * read at the end of h==799 sees the fresh line under its write-side
     * label. A repeat line never flips and reads the held bank. */
    logic [9:0] rd_next;
    always_comb rd_next = h + 10'd1;
    logic [10:0] lb_rd;
    always_comb lb_rd = h == 10'd799
        ? {flip_next ? wr_bank : !wr_bank, 10'd0}
        : {!wr_bank, rd_next};

    logic [15:0] lb_q;
    logic lb_blank;
    always_ff @(posedge clk) begin
        if (px_last) begin
            lb_q <= linebuf[lb_rd];
            lb_blank <= !(h == 10'd799 || h < cw - 10'd1);
        end
    end
    always_comb vid_mode_pix = lb_blank ? 16'h0000 : lb_q;
    always_comb vid_mode_filled = filled_q[!wr_bank];

    logic [9:0] t /*verilator public_flat_rd*/;
    logic [9:0] t_cv;
    logic render_now;
    always_comb begin
        t_cv = t - y_offset;
        render_now = !console && t >= y_offset && t < 10'd480
            && !(y_shift && t_cv[0]);
    end
    logic [8:0] t_row;
    always_comb t_row = y_shift ? t_cv[9:1] : t_cv[8:0];
    always_comb vid_mode_p_line = t_row;

    typedef enum logic [2:0] {
        S_IDLE, S_PROG, S_PROG_W, S_CFG, S_MODE
    } state_t;
    state_t state /*verilator public_flat_rd*/;

    logic [15:0] attr /*verilator public_flat_rd*/;
    logic [15:0] config_ptr /*verilator public_flat_rd*/;

    /* Sixteen bytes shifted in a halfword at a time, low half first and
     * entered at the top, so the config ends flush against bit 127 wherever
     * it started. A halfword base is a fifth fetch and a ninth shift, and
     * the junk halfword ahead of it falls out the bottom. vid_palram
     * forgives a halfword palette pointer the same way. */
    logic [2:0] cfg_i, cfg_n;
    logic [3:0] sh_c, sh_n;
    logic [127:0] cfgw;
    logic [15:0] hi_hold;
    logic hi_pend;
    logic gnt_d;
    logic [15:0] sh_in;
    always_comb sh_in = gnt_d ? a_rdata[15:0] : hi_hold;

    logic [9:0] cw;
    always_comb cw = x_shift ? 10'd320 : 10'd640;

    logic [2:0] mode_q;
    logic m3_start;
    logic m3_tl_start;
    logic [15:0] m3_pal_ptr;
    logic m3_pal_xram;
    logic [2:0] m3_bpp;
    logic m3_reversed;
    logic m3_seg_valid, m3_seg_imm;
    logic [22:0] m3_seg_bits;
    logic [9:0] m3_seg_px;
    logic m3_filled;
    logic tl_take;
    logic tl_a_req;
    logic [13:0] tl_a_addr;
    logic tl_pal_ld;
    logic [7:0] tl_pal_w;
    logic [8:0] tl_pal_words;
    logic [7:0] tl_pal_idx;
    logic tl_pal_xram, tl_pal_one_bpp;
    logic tl_px_we;
    logic [9:0] tl_px_addr;
    logic [15:0] tl_px_data;
    logic tl_done;
    logic m1_start;
    logic m1_a_req;
    logic [13:0] m1_a_addr;
    logic m1_tl_start;
    logic m1_seg_valid;
    logic [7:0] m1_seg_ibits;
    logic [15:0] m1_seg_fg, m1_seg_bg;
    logic [9:0] m1_seg_px;
    logic m1_filled;
    logic m2_start;
    logic m2_a_req;
    logic [13:0] m2_a_addr;
    logic m2_tl_start;
    logic [15:0] m2_pal_ptr;
    logic m2_pal_xram;
    logic [2:0] m2_bpp;
    logic m2_seg_valid, m2_seg_imm;
    logic [22:0] m2_seg_bits;
    logic [9:0] m2_seg_px;
    logic m2_filled;

    /* Only the mode holding the plane can be loading, so the write side
     * is a select rather than an arbiter. */
    logic m1_pal_ld;
    logic [7:0] m1_pal_w;
    logic [8:0] m1_pal_words;
    logic [7:0] m1_pal_idx_a, m1_pal_idx_b;
    logic m1_pal_xram;
    logic m1_pal_one_bpp;
    logic [15:0] pal_qa, pal_qb;
    logic pal_ld, pal_xram, pal_one_bpp;
    logic [7:0] pal_w, pal_idx_a;
    logic [8:0] pal_words;
    always_comb begin
        if (mode_q == 3'd1) begin
            pal_ld = m1_pal_ld;
            pal_w = m1_pal_w;
            pal_words = m1_pal_words;
            pal_idx_a = m1_pal_idx_a;
            pal_xram = m1_pal_xram;
            pal_one_bpp = m1_pal_one_bpp;
        end else begin
            pal_ld = tl_pal_ld;
            pal_w = tl_pal_w;
            pal_words = tl_pal_words;
            pal_idx_a = tl_pal_idx;
            pal_xram = tl_pal_xram;
            pal_one_bpp = tl_pal_one_bpp;
        end
    end
    vid_palram vid_palram (
        .clk(clk),
        .ld(pal_ld),
        .w(pal_w),
        .words(pal_words),
        .half(cfgw[97]),
        .a_rdata(a_rdata),
        .xram(pal_xram),
        .one_bpp(pal_one_bpp),
        .idx_a(pal_idx_a),
        .idx_b(m1_pal_idx_b),
        .vid_palram_qa(pal_qa),
        .vid_palram_qb(pal_qb)
    );

    vid_mode1 vid_mode1 (
        .clk(clk),
        .start(m1_start),
        .abort_i(line_start),
        .attr(attr),
        .cfgw(cfgw[127:0]),
        .t_row(t_row),
        .cw(cw),
        .vid_mode1_a_req(m1_a_req),
        .vid_mode1_a_addr(m1_a_addr),
        .a_gnt(a_gnt),
        .a_rdata(a_rdata),
        .vid_mode1_f_req(vid_mode_f_req),
        .vid_mode1_f_addr(vid_mode_f_addr),
        .f_gnt(f_gnt),
        .f_data(f_data),
        .vid_mode1_pal_ld(m1_pal_ld),
        .vid_mode1_pal_w(m1_pal_w),
        .vid_mode1_pal_words(m1_pal_words),
        .vid_mode1_pal_idx_a(m1_pal_idx_a),
        .vid_mode1_pal_idx_b(m1_pal_idx_b),
        .vid_mode1_pal_xram(m1_pal_xram),
        .vid_mode1_pal_one_bpp(m1_pal_one_bpp),
        .pal_qa(pal_qa),
        .pal_qb(pal_qb),
        .vid_mode1_tl_start(m1_tl_start),
        .vid_mode1_seg_valid(m1_seg_valid),
        .vid_mode1_seg_ibits(m1_seg_ibits),
        .vid_mode1_seg_fg(m1_seg_fg),
        .vid_mode1_seg_bg(m1_seg_bg),
        .vid_mode1_seg_px(m1_seg_px),
        .seg_take(tl_take),
        .vid_mode1_filled(m1_filled)
    );
    vid_mode2 vid_mode2 (
        .clk(clk),
        .start(m2_start),
        .abort_i(line_start),
        .attr(attr),
        .cfgw(cfgw[127:0]),
        .t_row(t_row),
        .cw(cw),
        .vid_mode2_a_req(m2_a_req),
        .vid_mode2_a_addr(m2_a_addr),
        .a_gnt(a_gnt && m2_a_req),
        .a_rdata(a_rdata),
        .vid_mode2_tl_start(m2_tl_start),
        .vid_mode2_pal_ptr(m2_pal_ptr),
        .vid_mode2_pal_xram(m2_pal_xram),
        .vid_mode2_bpp(m2_bpp),
        .vid_mode2_seg_valid(m2_seg_valid),
        .vid_mode2_seg_imm(m2_seg_imm),
        .vid_mode2_seg_bits(m2_seg_bits),
        .vid_mode2_seg_px(m2_seg_px),
        .seg_take(tl_take),
        .vid_mode2_filled(m2_filled)
    );
    vid_mode3 vid_mode3 (
        .clk(clk),
        .start(m3_start),
        .abort_i(line_start),
        .attr(attr),
        .cfgw(cfgw[111:0]),
        .t_row(t_row),
        .cw(cw),
        .vid_mode3_tl_start(m3_tl_start),
        .vid_mode3_pal_ptr(m3_pal_ptr),
        .vid_mode3_pal_xram(m3_pal_xram),
        .vid_mode3_bpp(m3_bpp),
        .vid_mode3_reversed(m3_reversed),
        .vid_mode3_seg_valid(m3_seg_valid),
        .vid_mode3_seg_imm(m3_seg_imm),
        .vid_mode3_seg_bits(m3_seg_bits),
        .vid_mode3_seg_px(m3_seg_px),
        .seg_take(tl_take),
        .vid_mode3_filled(m3_filled)
    );

    /* The tail's grants are only the cycles the front is not asking, so
     * the channel mux presents the front's address first. Mode 1's
     * segments are all immediate, so its grant line is silenced and the
     * front's fetches cannot churn the tail's ledgers. */
    logic tf_start;
    logic [15:0] tf_pal_ptr;
    logic tf_pal_xram;
    logic [2:0] tf_bpp;
    logic tf_reversed;
    logic tf_seg_valid, tf_seg_imm;
    logic [22:0] tf_seg_bits;
    logic [7:0] tf_seg_ibits;
    logic [15:0] tf_seg_fg, tf_seg_bg;
    logic [9:0] tf_seg_px;
    always_comb begin
        if (mode_q == 3'd1) begin
            tf_start = m1_tl_start;
            tf_pal_ptr = 16'd0;
            tf_pal_xram = 1'b0;
            tf_bpp = 3'd0;
            tf_reversed = 1'b0;
            tf_seg_valid = m1_seg_valid;
            tf_seg_imm = 1'b1;
            tf_seg_bits = 23'd0;
            tf_seg_ibits = m1_seg_ibits;
            tf_seg_fg = m1_seg_fg;
            tf_seg_bg = m1_seg_bg;
            tf_seg_px = m1_seg_px;
        end else if (mode_q == 3'd2) begin
            tf_start = m2_tl_start;
            tf_pal_ptr = m2_pal_ptr;
            tf_pal_xram = m2_pal_xram;
            tf_bpp = m2_bpp;
            tf_reversed = 1'b0;
            tf_seg_valid = m2_seg_valid;
            tf_seg_imm = m2_seg_imm;
            tf_seg_bits = m2_seg_bits;
            tf_seg_ibits = 8'd0;
            tf_seg_fg = 16'd0;
            tf_seg_bg = 16'd0;
            tf_seg_px = m2_seg_px;
        end else begin
            tf_start = m3_tl_start;
            tf_pal_ptr = m3_pal_ptr;
            tf_pal_xram = m3_pal_xram;
            tf_bpp = m3_bpp;
            tf_reversed = m3_reversed;
            tf_seg_valid = m3_seg_valid;
            tf_seg_imm = m3_seg_imm;
            tf_seg_bits = m3_seg_bits;
            tf_seg_ibits = 8'd0;
            tf_seg_fg = 16'd0;
            tf_seg_bg = 16'd0;
            tf_seg_px = m3_seg_px;
        end
    end
    vid_pixtail vid_pixtail (
        .clk(clk),
        .start(tf_start),
        .abort_i(line_start),
        .cw(cw),
        .pal_ptr(tf_pal_ptr),
        .pal_xram(tf_pal_xram),
        .bpp_log(tf_bpp),
        .reversed(tf_reversed),
        .seg_valid(tf_seg_valid),
        .seg_imm(tf_seg_imm),
        .seg_bits(tf_seg_bits),
        .seg_ibits(tf_seg_ibits),
        .seg_fg(tf_seg_fg),
        .seg_bg(tf_seg_bg),
        .seg_px(tf_seg_px),
        .vid_pixtail_seg_take(tl_take),
        .vid_pixtail_a_req(tl_a_req),
        .vid_pixtail_a_addr(tl_a_addr),
        .a_gnt(a_gnt && mode_q != 3'd1 && !m2_a_req),
        .a_rdy(1'b0),
        .a_rdata(a_rdata),
        .vid_pixtail_pal_ld(tl_pal_ld),
        .vid_pixtail_pal_w(tl_pal_w),
        .vid_pixtail_pal_words(tl_pal_words),
        .vid_pixtail_pal_idx(tl_pal_idx),
        .vid_pixtail_pal_xram(tl_pal_xram),
        .vid_pixtail_pal_one_bpp(tl_pal_one_bpp),
        .pal_q(pal_qa),
        .vid_pixtail_px_we(tl_px_we),
        .vid_pixtail_px_addr(tl_px_addr),
        .vid_pixtail_px_data(tl_px_data),
        .vid_pixtail_done(tl_done)
    );

    logic sub_a_req;
    logic [13:0] sub_a_addr;
    logic sub_px_we;
    logic [9:0] sub_px_addr;
    logic [15:0] sub_px_data;
    logic sub_done, sub_filled;
    always_comb begin
        if (mode_q == 3'd1) begin
            sub_a_req = m1_a_req;
            sub_a_addr = m1_a_addr;
            sub_px_we = tl_px_we;
            sub_px_addr = tl_px_addr;
            sub_px_data = tl_px_data;
            sub_done = tl_done;
            sub_filled = m1_filled;
        end else if (mode_q == 3'd2) begin
            sub_a_req = m2_a_req || tl_a_req;
            sub_a_addr = m2_a_req ? m2_a_addr : tl_a_addr;
            sub_px_we = tl_px_we;
            sub_px_addr = tl_px_addr;
            sub_px_data = tl_px_data;
            sub_done = tl_done;
            sub_filled = m2_filled;
        end else begin
            sub_a_req = tl_a_req;
            sub_a_addr = tl_a_addr;
            sub_px_we = tl_px_we;
            sub_px_addr = tl_px_addr;
            sub_px_data = tl_px_data;
            sub_done = tl_done;
            sub_filled = m3_filled;
        end
    end

    always_comb begin
        if (state == S_CFG) begin
            /* One word in flight: the half held back has to shift before
             * the next word's low half arrives. */
            vid_mode_a_req = cfg_i < cfg_n && !gnt_d;
            vid_mode_a_addr = config_ptr[15:2] + {11'd0, cfg_i};
        end else begin
            vid_mode_a_req = state == S_MODE && sub_a_req;
            vid_mode_a_addr = sub_a_addr;
        end
    end

    always_ff @(posedge clk) begin
        if (state == S_MODE && sub_px_we)
            linebuf[{wr_bank, sub_px_addr}] <= sub_px_data;
    end

    initial begin
        wr_bank = 1'b0;
        filled_q[0] = 1'b0;
        filled_q[1] = 1'b0;
        flip_next = 1'b0;
        state = S_IDLE;
        t = '0;
        attr = '0;
        config_ptr = '0;
        cfgw = '0;
        hi_hold = '0;
        hi_pend = 1'b0;
        cfg_i = '0;
        cfg_n = '0;
        sh_c = '0;
        sh_n = '0;
        m3_start = 1'b0;
        m2_start = 1'b0;
        m1_start = 1'b0;
        mode_q = '0;
        gnt_d = 1'b0;
    end
`ifdef VERILATOR
    /* Simulation's own reset, and the only thing in this module that
     * takes one — VERILATOR is defined by the simulator and by nothing
     * in the synthesis flow, so the fabric never sees the port. A
     * machine reset costs the beam a frame, because the beam does not
     * take one; this is what tells the stop below to expect it. */
    logic settled;
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n)
            settled <= 1'b0;
        else if (line_start && v == 10'd524)
            settled <= 1'b1;
`endif

    always_ff @(posedge clk) begin
        gnt_d <= a_gnt;
        m3_start <= 1'b0;
        m2_start <= 1'b0;
        m1_start <= 1'b0;
        /* The next line's pixel 0 is read during h==799, so the flip
         * must land before it or that pixel comes up stale. */
`ifdef VERILATOR
        /* The stop waits one frame, which is what a reset costs a beam
         * that does not take one — a frame of the black screen the
         * machine boots to. */
        if (settled && h == 10'd799 && state != S_IDLE)
            $fatal(1, "vid_mode underrun");
`endif
        if (line_start) begin
            t <= v == 10'd524 ? 10'd0 : v + 10'd1;
            if (flip_next)
                wr_bank <= !wr_bank;
            flip_next <= 1'b0;
            state <= S_PROG;
        end else begin
            case (state)
                S_IDLE: ;
                S_PROG: begin
                    if (!render_now)
                        state <= S_IDLE;
                    else if (p_gnt)
                        state <= S_PROG_W;
                end
                S_PROG_W: begin
                    attr <= p_entry[15:0];
                    config_ptr <= p_config;
                    mode_q <= p_entry[18:16];
                    if (!p_entry[31] || p_entry[18:16] == 3'd0
                        || p_entry[18:16] > 3'd3) begin
                        /* Marked unfilled and left unwritten: the
                         * compose skips an unfilled plane, and
                         * nothing else reads the buffer. */
                        state <= S_IDLE;
                        flip_next <= 1'b1;
                        filled_q[wr_bank] <= 1'b0;
                    end else begin
                        /* config_ptr takes p_config on this same clock. */
                        cfg_i <= '0;
                        sh_c <= '0;
                        hi_pend <= 1'b0;
                        cfg_n <= p_config[1] ? 3'd5 : 3'd4;
                        sh_n <= p_config[1] ? 4'd9 : 4'd8;
                        state <= S_CFG;
                    end
                end
                S_CFG: begin
                    if (a_gnt)
                        cfg_i <= cfg_i + 3'd1;
                    hi_pend <= gnt_d;
                    if (gnt_d)
                        hi_hold <= a_rdata[31:16];
                    if (gnt_d || hi_pend) begin
                        cfgw <= {sh_in, cfgw[127:16]};
                        sh_c <= sh_c + 4'd1;
                        if (sh_c + 4'd1 == sh_n) begin
                            if (mode_q == 3'd1)
                                m1_start <= 1'b1;
                            else if (mode_q == 3'd2)
                                m2_start <= 1'b1;
                            else
                                m3_start <= 1'b1;
                            state <= S_MODE;
                        end
                    end
                end
                S_MODE: begin
                    if (sub_done) begin
                        filled_q[wr_bank] <= sub_filled;
                        flip_next <= 1'b1;
                        state <= S_IDLE;
                    end
                end
                default: state <= S_IDLE;
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_mode;
    always_comb unused_vid_mode = ^{t_cv, p_entry[30:19], config_ptr[0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
