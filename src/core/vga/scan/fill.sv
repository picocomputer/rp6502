/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The fill engine, one for all three planes: the mode 1, 2 and 3
 * subengines, the row mapper and pixel tail they share, and the palette,
 * dispatched a plane at a time by sched. Running the fills serially is
 * safe because a mode that reads the palette reloads it first. The
 * subengine owns the XRAM channel and the pixel port until it reports
 * done.
 *
 * A line a mode rejects, blank or out of range, is emitted as a full
 * line of zeros, which compose reads as black on plane 0 and as
 * transparent on an overlay.
 */

module fill
    import mode::pal_fits;
(
    input logic clk,
    /* The start of a row of graphics. On a 320 wide canvas that is every
     * other line of timing, and a fill has the whole pair to finish in. */
    input logic row_start,

    /* sched holds the plane's mode, attributes and config from one start
     * to the next. */
    input logic start,
    input logic [2:0] mode,
    input logic [15:0] attr_i,
    input logic [15:0] config_ptr_i,

    input logic [8:0] t_row,
    input logic [9:0] cw,

    /* a_gnt means the address was taken; the word arrives on a_rdata two
     * clocks later. */
    output logic fill_a_req,
    output logic [13:0] fill_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    output logic fill_f_req,
    output logic [13:0] fill_f_addr,
    input logic f_gnt,
    input logic [7:0] f_data,

    /* A pair a clock, as pixtail.sv emits and linebuf.sv lands it. */
    output logic [1:0] fill_px_we,
    output logic [9:0] fill_px_addr,
    output logic [31:0] fill_px_data,

    output logic fill_done
);

    typedef enum logic [1:0] {
        F_IDLE, F_CFG, F_MODE
    } state_t;
    state_t state /*verilator public_flat_rd*/;


    /* Halfwords shift down, so the config's first halfword lands at bit
     * 0 wherever it started: a halfword-aligned config takes nine shifts,
     * and the junk halfword ahead of it falls off the bottom. */
    logic [2:0] cfg_i, cfg_n;
    logic [3:0] sh_c, sh_n;
    logic [127:0] cfgw;
    logic [15:0] hi_hold;
    logic hi_pend;
    logic gnt_d1, gnt_d;
    logic [15:0] sh_in;
    always_comb sh_in = gnt_d ? a_rdata[15:0] : hi_hold;

    logic m3_start;
    logic [2:0] m3_bpp;
    logic m3_reversed;
    logic m3_seg_imm;
    logic [22:0] m3_seg_bits;
    logic tl_take;
    logic tl_a_req;
    logic [13:0] tl_a_addr;
    logic tl_pal_ld, tl_pal_done;
    logic [7:0] tl_pal_idx, tl_pal_idx1;
    logic tl_pal_xram, tl_pal_one_bpp;
    logic [1:0] tl_px_we;
    logic [9:0] tl_px_addr;
    logic [31:0] tl_px_data;
    logic tl_done;
    logic m1_start;
    logic m1_a_req;
    logic [13:0] m1_a_addr;
    logic [7:0] m1_seg_ibits;
    logic [15:0] m1_seg_fg, m1_seg_bg;
    logic m2_start;
    logic m2_a_req;
    logic [13:0] m2_a_addr;
    logic [2:0] m2_bpp;
    logic m2_seg_imm;
    logic [22:0] m2_seg_bits;

    /* One row mapper for the three modes, which never run at once: each
     * names its window and follows the walk, and the one the plane is in
     * is heard. */
    logic [4:0] m1_win_wf, m1_win_hf, m2_win_wf, m2_win_hf;
    logic [4:0] m3_win_wf, m3_win_hf;
    logic [19:0] m1_sizeof_row, m2_sizeof_row, m3_sizeof_row;
    logic m1_addr, m2_addr, m3_addr, m1_seg_on, m2_seg_on, m3_seg_on;
    logic [14:0] m1_data_row, m2_data_row, m3_data_row;
    logic m1_run_ready, m2_run_ready;
    logic [9:0] m1_run_max, m2_run_max;
    logic [4:0] rm_win_wf, rm_win_hf;
    logic [19:0] rm_sizeof_row;
    logic rm_addr, rm_seg_on, rm_run_ready;
    logic [14:0] rm_data_row;
    logic [9:0] rm_run_max;
    always_comb begin
        if (mode == 3'd1) begin
            rm_win_wf = m1_win_wf;
            rm_win_hf = m1_win_hf;
            rm_sizeof_row = m1_sizeof_row;
            rm_addr = m1_addr;
            rm_data_row = m1_data_row;
            rm_seg_on = m1_seg_on;
            rm_run_ready = m1_run_ready;
            rm_run_max = m1_run_max;
        end else if (mode == 3'd2) begin
            rm_win_wf = m2_win_wf;
            rm_win_hf = m2_win_hf;
            rm_sizeof_row = m2_sizeof_row;
            rm_addr = m2_addr;
            rm_data_row = m2_data_row;
            rm_seg_on = m2_seg_on;
            rm_run_ready = m2_run_ready;
            rm_run_max = m2_run_max;
        end else begin
            /* A bitmap's run is one segment, bounded by the line. */
            rm_win_wf = m3_win_wf;
            rm_win_hf = m3_win_hf;
            rm_sizeof_row = m3_sizeof_row;
            rm_addr = m3_addr;
            rm_data_row = m3_data_row;
            rm_seg_on = m3_seg_on;
            rm_run_ready = 1'b1;
            rm_run_max = cw;
        end
    end
    logic rm_settle, rm_reject, rm_blank, rm_overrun;
    logic signed [16:0] rm_row, rm_col;
    logic [16:0] rm_row_base;
    logic rm_seg_valid;
    logic [9:0] rm_seg_px;
    logic rm_run, rm_right, rm_wrap, rm_end;
    rowmap rowmap (
        .clk(clk),
        .start(m1_start || m2_start || m3_start),
        .abort_i(row_start),
        .cfgw(cfgw[95:0]),
        .t_row(t_row),
        .cw(cw),
        .win_wf(rm_win_wf),
        .win_hf(rm_win_hf),
        .sizeof_row(rm_sizeof_row),
        .rowmap_settle(rm_settle),
        .rowmap_reject(rm_reject),
        .rowmap_row(rm_row),
        .rowmap_col(rm_col),
        .rowmap_blank(rm_blank),
        .rowmap_overrun(rm_overrun),
        .addr(rm_addr),
        .data_row(rm_data_row),
        .rowmap_row_base(rm_row_base),
        .seg_on(rm_seg_on),
        .run_ready(rm_run_ready),
        .run_max(rm_run_max),
        .seg_take(tl_take),
        .rowmap_seg_valid(rm_seg_valid),
        .rowmap_seg_px(rm_seg_px),
        .rowmap_run(rm_run),
        .rowmap_right(rm_right),
        .rowmap_wrap(rm_wrap),
        .rowmap_end(rm_end)
    );

    /* The tail loads the palette for every mode, a word a landing, and
     * the landings are counted here from the fill's start. */
    logic [2:0] m1_bpp;
    logic [7:0] m1_pal_idx_a, m1_pal_idx_b;
    logic [15:0] pal_qa, pal_qb;
    logic [7:0] pal_w;
    initial pal_w = '0;
    always_ff @(posedge clk)
        if (start)
            pal_w <= '0;
        else if (tl_pal_ld)
            pal_w <= pal_w + 8'd1;
    palram palram (
        .clk(clk),
        .ld(tl_pal_ld),
        .w(pal_w),
        .half(cfgw[97]),
        .a_rdata(a_rdata),
        .xram(tl_pal_xram),
        .one_bpp(tl_pal_one_bpp),
        .idx_a(mode == 3'd1 ? m1_pal_idx_a : tl_pal_idx),
        .idx_b(mode == 3'd1 ? m1_pal_idx_b : tl_pal_idx1),
        .palram_qa(pal_qa),
        .palram_qb(pal_qb)
    );

    mode1 mode1 (
        .clk(clk),
        .start(m1_start),
        .abort_i(row_start),
        .attr(attr_i),
        .cfgw(cfgw[127:0]),
        .mode1_win_wf(m1_win_wf),
        .mode1_win_hf(m1_win_hf),
        .mode1_sizeof_row(m1_sizeof_row),
        .mode1_addr(m1_addr),
        .mode1_data_row(m1_data_row),
        .mode1_seg_on(m1_seg_on),
        .mode1_run_ready(m1_run_ready),
        .mode1_run_max(m1_run_max),
        .rm_settle(rm_settle),
        .rm_row(rm_row),
        .rm_col(rm_col),
        .rm_row_base(rm_row_base),
        .rm_blank(rm_blank),
        .rm_overrun(rm_overrun),
        .rm_run(rm_run),
        .rm_wrap(rm_wrap),
        .rm_end(rm_end),
        .mode1_a_req(m1_a_req),
        .mode1_a_addr(m1_a_addr),
        .a_gnt(a_gnt),
        .a_rdata(a_rdata),
        .mode1_f_req(fill_f_req),
        .mode1_f_addr(fill_f_addr),
        .f_gnt(f_gnt),
        .f_data(f_data),
        .mode1_bpp(m1_bpp),
        .pal_done(tl_pal_done),
        .mode1_pal_idx_a(m1_pal_idx_a),
        .mode1_pal_idx_b(m1_pal_idx_b),
        .pal_qa(pal_qa),
        .pal_qb(pal_qb),
        .mode1_seg_ibits(m1_seg_ibits),
        .mode1_seg_fg(m1_seg_fg),
        .mode1_seg_bg(m1_seg_bg),
        .seg_take(tl_take)
    );
    mode2 mode2 (
        .clk(clk),
        .start(m2_start),
        .abort_i(row_start),
        .attr(attr_i),
        .cfgw(cfgw[127:0]),
        .mode2_win_wf(m2_win_wf),
        .mode2_win_hf(m2_win_hf),
        .mode2_sizeof_row(m2_sizeof_row),
        .mode2_addr(m2_addr),
        .mode2_data_row(m2_data_row),
        .mode2_seg_on(m2_seg_on),
        .mode2_run_ready(m2_run_ready),
        .mode2_run_max(m2_run_max),
        .rm_settle(rm_settle),
        .rm_reject(rm_reject),
        .rm_row(rm_row),
        .rm_col(rm_col),
        .rm_row_base(rm_row_base),
        .rm_blank(rm_blank),
        .rm_overrun(rm_overrun),
        .rm_run(rm_run),
        .rm_right(rm_right),
        .rm_wrap(rm_wrap),
        .rm_end(rm_end),
        .mode2_a_req(m2_a_req),
        .mode2_a_addr(m2_a_addr),
        .a_gnt(a_gnt && m2_a_req),
        .a_rdata(a_rdata),
        .mode2_bpp(m2_bpp),
        .mode2_seg_imm(m2_seg_imm),
        .mode2_seg_bits(m2_seg_bits),
        .seg_take(tl_take)
    );
    mode3 mode3 (
        .clk(clk),
        .start(m3_start),
        .abort_i(row_start),
        .attr(attr_i),
        .cfgw(cfgw[111:0]),
        .mode3_win_wf(m3_win_wf),
        .mode3_win_hf(m3_win_hf),
        .mode3_sizeof_row(m3_sizeof_row),
        .mode3_addr(m3_addr),
        .mode3_data_row(m3_data_row),
        .mode3_seg_on(m3_seg_on),
        .rm_settle(rm_settle),
        .rm_row(rm_row),
        .rm_col(rm_col),
        .rm_row_base(rm_row_base),
        .rm_run(rm_run),
        .rm_end(rm_end),
        .mode3_bpp(m3_bpp),
        .mode3_reversed(m3_reversed),
        .mode3_seg_imm(m3_seg_imm),
        .mode3_seg_bits(m3_seg_bits)
    );

    /* The tail is granted only the clocks the fronts are not asking for,
     * so the channel mux presents a front's address first and a front's
     * fetches never reach the tail's count of words in flight. */
    /* The tail's plan, the same for every mode: the palette is the
     * config's, loaded when the line is not blank and the depth indexes
     * one that fits in XRAM. It starts on the mode's address clock, where
     * the overrun blanks a line too. */
    logic [2:0] tf_bpp;
    logic tf_reversed, tf_pal_xram;
    always_comb begin
        tf_bpp = mode == 3'd1 ? m1_bpp : mode == 3'd2 ? m2_bpp : m3_bpp;
        tf_reversed = mode != 3'd1 && mode != 3'd2 && m3_reversed;
    end
    initial tf_pal_xram = 1'b0;
    always_ff @(posedge clk)
        if (rm_addr)
            tf_pal_xram <= !rm_blank && !rm_overrun
                && !cfgw[96] && !tf_bpp[2]
                && pal_fits(cfgw[111:96], tf_bpp[1:0]);
    logic tf_seg_imm;
    logic [39:0] tf_seg_pay;
    always_comb begin
        if (mode == 3'd1) begin
            tf_seg_imm = 1'b1;
            tf_seg_pay = {m1_seg_ibits, m1_seg_fg, m1_seg_bg};
        end else if (mode == 3'd2) begin
            tf_seg_imm = m2_seg_imm;
            tf_seg_pay = {17'd0, m2_seg_bits};
        end else begin
            tf_seg_imm = m3_seg_imm;
            tf_seg_pay = {17'd0, m3_seg_bits};
        end
    end
    pixtail pixtail (
        .clk(clk),
        .start(rm_addr),
        .abort_i(row_start),
        .cw(cw),
        .pal_ptr(cfgw[111:96]),
        .pal_xram(tf_pal_xram),
        .bpp_log(tf_bpp),
        .reversed(tf_reversed),
        .seg_valid(rm_seg_valid),
        .seg_imm(tf_seg_imm),
        .seg_pay(tf_seg_pay),
        .seg_px(rm_seg_px),
        .pixtail_seg_take(tl_take),
        .pixtail_a_req(tl_a_req),
        .pixtail_a_addr(tl_a_addr),
        .a_gnt(a_gnt && !m1_a_req && !m2_a_req),
        .a_rdata(a_rdata),
        .pixtail_pal_ld(tl_pal_ld),
        .pixtail_pal_done(tl_pal_done),
        .pixtail_pal_idx(tl_pal_idx),
        .pixtail_pal_idx1(tl_pal_idx1),
        .pixtail_pal_xram(tl_pal_xram),
        .pixtail_pal_one_bpp(tl_pal_one_bpp),
        .pal_q(pal_qa),
        .pal_q1(pal_qb),
        .pixtail_px_we(tl_px_we),
        .pixtail_px_addr(tl_px_addr),
        .pixtail_px_data(tl_px_data),
        .pixtail_done(tl_done)
    );

    /* A front asks only while the mode it belongs to runs. */
    logic sub_a_req;
    logic [13:0] sub_a_addr;
    always_comb begin
        sub_a_req = m1_a_req || m2_a_req || tl_a_req;
        sub_a_addr = m1_a_req ? m1_a_addr : m2_a_req ? m2_a_addr : tl_a_addr;
    end

    always_comb begin
        if (state == F_CFG) begin
            /* One word is asked for every other clock, because the half
             * held back has to shift before the next word's low half
             * arrives. */
            fill_a_req = cfg_i < cfg_n && !gnt_d1;
            fill_a_addr = config_ptr_i[15:2] + {11'd0, cfg_i};
        end else begin
            fill_a_req = state == F_MODE && sub_a_req;
            fill_a_addr = sub_a_addr;
        end
    end

    always_comb begin
        fill_px_we = state == F_MODE ? tl_px_we : 2'b00;
        fill_px_addr = tl_px_addr;
        fill_px_data = tl_px_data;
        fill_done = state == F_MODE && tl_done;
    end

    initial begin
        state = F_IDLE;
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
        gnt_d1 = 1'b0;
        gnt_d = 1'b0;
    end
    always_ff @(posedge clk) begin
        gnt_d1 <= a_gnt;
        gnt_d <= gnt_d1;
        m3_start <= 1'b0;
        m2_start <= 1'b0;
        m1_start <= 1'b0;
        if (row_start)
            state <= F_IDLE;
        else if (start) begin
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
                            if (mode == 3'd1)
                                m1_start <= 1'b1;
                            else if (mode == 3'd2)
                                m2_start <= 1'b1;
                            else
                                m3_start <= 1'b1;
                            state <= F_MODE;
                        end
                    end
                end
                F_MODE: begin
                    if (tl_done)
                        state <= F_IDLE;
                end
                default: state <= F_IDLE;
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_fill;
    always_comb unused_fill = ^{config_ptr_i[1:0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
