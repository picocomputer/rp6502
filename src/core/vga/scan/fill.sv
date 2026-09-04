/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The fill engine, one for all three planes: the mode 1/2/3 subengines,
 * the shared pixel tail, and the palette snapshot, dispatched a plane at
 * a time by sched. Serial fills are safe by the palette's own
 * contract — a mode that reads the palette always reloads it first —
 * and by the line buffers living outside: this engine only ever writes
 * the plane it was dispatched to. The subengine owns the XRAM channel
 * and the pixel port until it reports done.
 *
 * A rejected line — blank, out of range — is emitted as a full line of
 * padding zeros, and the compose leans on that: zeros ARE the unfilled
 * line, black under the base plane's rule, transparent above.
 */

module fill (
    input logic clk,
    input logic line_start,

    input logic start,
    input logic [2:0] mode,
    input logic [15:0] attr_i,
    input logic [15:0] config_ptr_i,

    input logic [8:0] t_row,
    input logic [9:0] cw,

    /* gnt means the address was taken; the word arrives next clock. */
    output logic fill_a_req,
    output logic [13:0] fill_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    output logic fill_f_req,
    output logic [13:0] fill_f_addr,
    input logic f_gnt,
    input logic [7:0] f_data,

    output logic fill_px_we,
    output logic [9:0] fill_px_addr,
    output logic [15:0] fill_px_data,

    output logic fill_done
);

    typedef enum logic [1:0] {
        F_IDLE, F_CFG, F_MODE
    } state_t;
    state_t state /*verilator public_flat_rd*/;

    logic [15:0] attr;
    logic [15:0] config_ptr;
    logic [2:0] mode_q;

    /* Entered at the top, so the config ends flush against bit 127
     * wherever it started and the junk halfword ahead of it falls out
     * the bottom. */
    logic [2:0] cfg_i, cfg_n;
    logic [3:0] sh_c, sh_n;
    logic [127:0] cfgw;
    logic [15:0] hi_hold;
    logic hi_pend;
    logic gnt_d;
    logic [15:0] sh_in;
    always_comb sh_in = gnt_d ? a_rdata[15:0] : hi_hold;

    logic m3_start;
    logic m3_tl_start;
    logic [15:0] m3_pal_ptr;
    logic m3_pal_xram;
    logic [2:0] m3_bpp;
    logic m3_reversed;
    logic m3_seg_valid, m3_seg_imm;
    logic [22:0] m3_seg_bits;
    logic [9:0] m3_seg_px;
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

    /* Only the mode holding the engine can be loading, so the write side
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
    palram palram (
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
        .palram_qa(pal_qa),
        .palram_qb(pal_qb)
    );

    mode1 mode1 (
        .clk(clk),
        .start(m1_start),
        .abort_i(line_start),
        .attr(attr),
        .cfgw(cfgw[127:0]),
        .t_row(t_row),
        .cw(cw),
        .mode1_a_req(m1_a_req),
        .mode1_a_addr(m1_a_addr),
        .a_gnt(a_gnt),
        .a_rdata(a_rdata),
        .mode1_f_req(fill_f_req),
        .mode1_f_addr(fill_f_addr),
        .f_gnt(f_gnt),
        .f_data(f_data),
        .mode1_pal_ld(m1_pal_ld),
        .mode1_pal_w(m1_pal_w),
        .mode1_pal_words(m1_pal_words),
        .mode1_pal_idx_a(m1_pal_idx_a),
        .mode1_pal_idx_b(m1_pal_idx_b),
        .mode1_pal_xram(m1_pal_xram),
        .mode1_pal_one_bpp(m1_pal_one_bpp),
        .pal_qa(pal_qa),
        .pal_qb(pal_qb),
        .mode1_tl_start(m1_tl_start),
        .mode1_seg_valid(m1_seg_valid),
        .mode1_seg_ibits(m1_seg_ibits),
        .mode1_seg_fg(m1_seg_fg),
        .mode1_seg_bg(m1_seg_bg),
        .mode1_seg_px(m1_seg_px),
        .seg_take(tl_take)
    );
    mode2 mode2 (
        .clk(clk),
        .start(m2_start),
        .abort_i(line_start),
        .attr(attr),
        .cfgw(cfgw[127:0]),
        .t_row(t_row),
        .cw(cw),
        .mode2_a_req(m2_a_req),
        .mode2_a_addr(m2_a_addr),
        .a_gnt(a_gnt && m2_a_req),
        .a_rdata(a_rdata),
        .mode2_tl_start(m2_tl_start),
        .mode2_pal_ptr(m2_pal_ptr),
        .mode2_pal_xram(m2_pal_xram),
        .mode2_bpp(m2_bpp),
        .mode2_seg_valid(m2_seg_valid),
        .mode2_seg_imm(m2_seg_imm),
        .mode2_seg_bits(m2_seg_bits),
        .mode2_seg_px(m2_seg_px),
        .seg_take(tl_take)
    );
    mode3 mode3 (
        .clk(clk),
        .start(m3_start),
        .abort_i(line_start),
        .attr(attr),
        .cfgw(cfgw[111:0]),
        .t_row(t_row),
        .cw(cw),
        .mode3_tl_start(m3_tl_start),
        .mode3_pal_ptr(m3_pal_ptr),
        .mode3_pal_xram(m3_pal_xram),
        .mode3_bpp(m3_bpp),
        .mode3_reversed(m3_reversed),
        .mode3_seg_valid(m3_seg_valid),
        .mode3_seg_imm(m3_seg_imm),
        .mode3_seg_bits(m3_seg_bits),
        .mode3_seg_px(m3_seg_px),
        .seg_take(tl_take)
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
    pixtail pixtail (
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
        .pixtail_seg_take(tl_take),
        .pixtail_a_req(tl_a_req),
        .pixtail_a_addr(tl_a_addr),
        .a_gnt(a_gnt && mode_q != 3'd1 && !m2_a_req),
        .a_rdy(1'b0),
        .a_rdata(a_rdata),
        .pixtail_pal_ld(tl_pal_ld),
        .pixtail_pal_w(tl_pal_w),
        .pixtail_pal_words(tl_pal_words),
        .pixtail_pal_idx(tl_pal_idx),
        .pixtail_pal_xram(tl_pal_xram),
        .pixtail_pal_one_bpp(tl_pal_one_bpp),
        .pal_q(pal_qa),
        .pixtail_px_we(tl_px_we),
        .pixtail_px_addr(tl_px_addr),
        .pixtail_px_data(tl_px_data),
        .pixtail_done(tl_done)
    );

    logic sub_a_req;
    logic [13:0] sub_a_addr;
    logic sub_px_we;
    logic [9:0] sub_px_addr;
    logic [15:0] sub_px_data;
    logic sub_done;
    always_comb begin
        if (mode_q == 3'd1) begin
            sub_a_req = m1_a_req;
            sub_a_addr = m1_a_addr;
            sub_px_we = tl_px_we;
            sub_px_addr = tl_px_addr;
            sub_px_data = tl_px_data;
            sub_done = tl_done;
        end else if (mode_q == 3'd2) begin
            sub_a_req = m2_a_req || tl_a_req;
            sub_a_addr = m2_a_req ? m2_a_addr : tl_a_addr;
            sub_px_we = tl_px_we;
            sub_px_addr = tl_px_addr;
            sub_px_data = tl_px_data;
            sub_done = tl_done;
        end else begin
            sub_a_req = tl_a_req;
            sub_a_addr = tl_a_addr;
            sub_px_we = tl_px_we;
            sub_px_addr = tl_px_addr;
            sub_px_data = tl_px_data;
            sub_done = tl_done;
        end
    end

    always_comb begin
        if (state == F_CFG) begin
            /* One word in flight: the half held back has to shift before
             * the next word's low half arrives. */
            fill_a_req = cfg_i < cfg_n && !gnt_d;
            fill_a_addr = config_ptr[15:2] + {11'd0, cfg_i};
        end else begin
            fill_a_req = state == F_MODE && sub_a_req;
            fill_a_addr = sub_a_addr;
        end
    end

    always_comb begin
        fill_px_we = state == F_MODE && sub_px_we;
        fill_px_addr = sub_px_addr;
        fill_px_data = sub_px_data;
        fill_done = state == F_MODE && sub_done;
    end

    initial begin
        state = F_IDLE;
        attr = '0;
        config_ptr = '0;
        mode_q = '0;
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
        gnt_d = 1'b0;
    end
    always_ff @(posedge clk) begin
        gnt_d <= a_gnt;
        m3_start <= 1'b0;
        m2_start <= 1'b0;
        m1_start <= 1'b0;
        if (line_start)
            state <= F_IDLE;
        else if (start) begin
            attr <= attr_i;
            config_ptr <= config_ptr_i;
            mode_q <= mode;
            cfg_i <= '0;
            sh_c <= '0;
            hi_pend <= 1'b0;
            cfg_n <= config_ptr_i[1] ? 3'd5 : 3'd4;
            sh_n <= config_ptr_i[1] ? 4'd9 : 4'd8;
            state <= F_CFG;
        end else begin
            case (state)
                F_IDLE: ;
                F_CFG: begin
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
                            state <= F_MODE;
                        end
                    end
                end
                F_MODE: begin
                    if (sub_done)
                        state <= F_IDLE;
                end
                default: state <= F_IDLE;
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_fill;
    always_comb unused_fill = ^{config_ptr[1:0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
