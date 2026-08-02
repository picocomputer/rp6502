/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One plane's line engine: the scaffolding every mode shares, working one
 * raster line ahead of the beam into a ping-pong line buffer — the
 * vid_term discipline generalized. Each line: wait for this plane's prog
 * slot, fetch the config words, and hand the line to the mode's own
 * pipeline — vid_mode3 so far, vid_mode1 and vid_mode2 beside it as they
 * land, the modes.h-and-mode files split of the C carried into RTL. The
 * subengine owns the XRAM channel and the pixel port until it reports
 * done and whether the plane counts as filled.
 *
 * On line-doubled canvases a row renders once, on the raster line where
 * it first appears, and the buffer holds through the repeat — scanvideo's
 * y_scale semantics, never a re-render the oracle could not have seen.
 *
 * XRAM port A and the prog-table read port are shared three ways; grants
 * rotate and the pipelines absorb the latency.
 */

module vid_mode (
    input logic clk,
    input logic rst_n,

    input logic [9:0] v,
    input logic [9:0] h,
    input logic px_last,
    input logic line_start,

    /* Latched canvas geometry from vid_prog. On the console canvas the
     * planes idle — the oracle's fill guard — which also shields the
     * unreset prog BRAM across a warm reset. */
    input logic console,
    input logic x_shift,
    input logic y_shift,
    input logic [9:0] y_offset,

    /* This plane's prog entry: request granted in this plane's slot, the
     * entry registered the clock after. */
    output logic [8:0] vid_mode_p_line,  /* comb: valid whenever granted */
    input logic p_gnt,
    input logic [31:0] p_entry,
    input logic [15:0] p_config,

    /* XRAM port A, arbitrated: hold req with an address; gnt means the
     * address was taken and the word arrives next clock. */
    output logic vid_mode_a_req,
    output logic [13:0] vid_mode_a_addr,
    input logic a_gnt,

    /* The font store, for mode 1's built-in glyphs. */
    output logic vid_mode_f_req,
    output logic [13:0] vid_mode_f_addr,
    input logic f_gnt,
    input logic [7:0] f_data,
    input logic [31:0] a_rdata,

    /* The beam side: this plane's pixel at h, and whether the line
     * filled at all. */
    output logic [15:0] vid_mode_pix,
    output logic vid_mode_filled,

    /* The sprite stage: the fill's outcome for the line in progress,
     * writes into the working bank after the fill, and the claim that
     * turns a zeroed bank into a filled layer. */
    output logic vid_mode_busy,
    output logic vid_mode_rnew,
    output logic vid_mode_rfilled,
    /* One pulse for a fill that did not finish before the beam wanted
     * it. The simulation stops on this; hardware cannot, so it counts
     * instead — otherwise a plane that misses its deadline simply
     * re-scans the line before it, with nothing to say so. */
    output logic vid_mode_underrun,
    input logic sp_we,
    input logic [9:0] sp_addr,
    input logic [15:0] sp_data,
    input logic sp_force
);

    /* The bank rides inside the address and the output register
     * carries only the buffer; either one broken keeps the whole line
     * out of block memory. */
    (* ramstyle = "no_rw_check" *)
    logic [15:0] linebuf[2048];
    logic wr_bank;
    logic filled_q[2] /*verilator public_flat_rd*/;
    logic flip_next;

    /* The beam reads one ahead of itself, on each pixel's last tick. The
     * bank flip lands on h==0's first tick, so only the pixel-0 read at
     * the end of h==799 still sees the fresh line under its write-side
     * label; a repeat line never flips and reads the held bank. */
    /* Native scanout: address h reads pixel h. This used to halve the
     * address on 320-wide canvases so each stored pixel spanned two beam
     * slots; the machine emits each pixel once now, and a platform whose
     * sink wants the repeat makes it downstream, where it is free. */
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
    /* The flip lands on h==0's first tick, so everywhere the beam shows
     * pixels the displayed line is the read-side bank — and flip_next
     * already belongs to the line being rendered. */
    always_comb vid_mode_filled = filled_q[!wr_bank];

    always_comb vid_mode_rnew = flip_next;
    always_comb vid_mode_rfilled = filled_q[wr_bank];

    /* Target line: the canvas row it maps to, and whether this raster
     * line starts that row's render. */
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
    always_comb vid_mode_busy = state != S_IDLE;

    typedef enum logic [2:0] {
        S_IDLE, S_PROG, S_PROG_W, S_CFG, S_MODE
    } state_t;
    state_t state /*verilator public_flat_rd*/;

    logic [15:0] attr /*verilator public_flat_rd*/;
    logic [15:0] config_ptr /*verilator public_flat_rd*/;

    /* Five fetched words hold any mode's config — mode 1's 16 bytes from
     * a halfword-aligned pointer span them all, and every pipeline reads
     * its bytes at fixed offsets from here.
     *
     * The words land already aligned. The halfword offset is settled in
     * S_PROG_W, a state before the first read is even issued, so the
     * shift belongs on the capture rather than in front of every
     * pipeline's arithmetic — where it was one register bit driving a
     * hundred and ninety-four loads, and the first hop of the worst
     * path in six different blocks. */
    logic [2:0] cfg_i, cfg_c;
    logic [143:0] cfgw;

    logic [9:0] cw;
    always_comb cw = x_shift ? 10'd320 : 10'd640;

    /* The mode pipelines; the prog entry's mode bits pick one. */
    logic [2:0] mode_q;
    logic m3_start;
    /* Modes 2 and 3 are fronts on the shared pixel tail: each describes
     * the line as segments and the tail does the rest. The channel,
     * pixels and palette below are the tail's. */
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

    /* One palette for the plane. Only the mode holding it can be
     * loading, so the write side is a select rather than an arbiter. */
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
        .rst_n(rst_n),
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
        .rst_n(rst_n),
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
        .rst_n(rst_n),
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

    /* The shared pixel tail behind whichever front is running. Mode 2
     * keeps fetching map bytes while the tail streams pixel words, so
     * the tail's grants are only the cycles the front is not asking —
     * the wrapper's channel mux presents the front's address first.
     * Mode 1's segments are all immediate and its palette is its own,
     * so during it the tail touches no memory at all: its grant line
     * is silenced so the front's fetches cannot churn its ledgers. */
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
        .rst_n(rst_n),
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

    /* The running pipeline's channel, pixel port, and completion. */
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

    /* The XRAM channel: the plane's own config fetch, or the pipeline's. */
    always_comb begin
        if (state == S_CFG) begin
            vid_mode_a_req = cfg_i < 3'd5;
            vid_mode_a_addr = config_ptr[15:2] + {11'd0, cfg_i};
        end else begin
            vid_mode_a_req = state == S_MODE && sub_a_req;
            vid_mode_a_addr = sub_a_addr;
        end
    end

    /* The write bank: the pipeline's pixels, or the sprite stage
     * painting after the fill went idle. */
    always_ff @(posedge clk) begin
        if (state == S_MODE && sub_px_we)
            linebuf[{wr_bank, sub_px_addr}] <= sub_px_data;
        else if (sp_we)
            linebuf[{wr_bank, sp_addr}] <= sp_data;
    end

    logic gnt_d;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wr_bank <= 1'b0;
            filled_q[0] <= 1'b0;
            filled_q[1] <= 1'b0;
            flip_next <= 1'b0;
            vid_mode_underrun <= 1'b0;
            state <= S_IDLE;
            t <= '0;
            attr <= '0;
            config_ptr <= '0;
            cfgw <= '0;
            cfg_i <= '0;
            cfg_c <= '0;
            m3_start <= 1'b0;
            m2_start <= 1'b0;
            m1_start <= 1'b0;
            mode_q <= '0;
            gnt_d <= 1'b0;
        end else begin
            gnt_d <= a_gnt;
            m3_start <= 1'b0;
            m2_start <= 1'b0;
            m1_start <= 1'b0;
            if (sp_force) begin
                flip_next <= 1'b1;
                filled_q[wr_bank] <= 1'b1;
            end
            /* The beam's true deadline: the next line's pixel 0 is read
             * during h==799, so the flip must have landed before it. A
             * completion during h==799 would silently scan a stale first
             * pixel; the pipelines' own aborts fire later. */
            vid_mode_underrun <= h == 10'd799 && state != S_IDLE;
`ifndef SYNTHESIS
            if (h == 10'd799 && state != S_IDLE)
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
                        /* t settled last clock; wait for this plane's
                         * prog slot. */
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
                            /* Nothing programmed, or a mode whose
                             * pipeline is not built yet. The buffer is
                             * marked unfilled and vid_compose skips an
                             * unfilled plane, so there is nothing to
                             * write: this used to walk the whole line
                             * putting zeros into a buffer no one would
                             * read, six hundred and forty clocks an
                             * idle plane, every line. The one reader
                             * that would care is the sprite stage
                             * painting a plane that never filled, and
                             * it clears the buffer itself. */
                            state <= S_IDLE;
                            flip_next <= 1'b1;
                            filled_q[wr_bank] <= 1'b0;
                        end else begin
                            cfg_i <= '0;
                            cfg_c <= '0;
                            state <= S_CFG;
                        end
                    end
                    S_CFG: begin
                        /* Issue on grant, capture the clock after. */
                        if (a_gnt)
                            cfg_i <= cfg_i + 3'd1;
                        if (gnt_d) begin
                            if (config_ptr[1])
                                case (cfg_c)
                                    3'd0: cfgw[15:0] <= a_rdata[31:16];
                                    3'd1: cfgw[47:16] <= a_rdata;
                                    3'd2: cfgw[79:48] <= a_rdata;
                                    3'd3: cfgw[111:80] <= a_rdata;
                                    default: cfgw[143:112] <= a_rdata;
                                endcase
                            else
                                case (cfg_c)
                                    3'd0: cfgw[31:0] <= a_rdata;
                                    3'd1: cfgw[63:32] <= a_rdata;
                                    3'd2: cfgw[95:64] <= a_rdata;
                                    3'd3: cfgw[127:96] <= a_rdata;
                                    default: cfgw[143:128] <= a_rdata[15:0];
                                endcase
                            cfg_c <= cfg_c + 3'd1;
                            if (cfg_c == 3'd4) begin
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
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_mode;
    always_comb unused_vid_mode = ^{t_cv, p_entry[30:19], cfgw[143:128],
                                    config_ptr[0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
