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
    input logic sp_we,
    input logic [9:0] sp_addr,
    input logic [15:0] sp_data,
    input logic sp_force
);

    logic [15:0] linebuf[2][640];
    logic wr_bank;
    logic filled_q[2] /*verilator public_flat_rd*/;
    logic flip_next;

    /* The beam reads one ahead of itself, on each pixel's last tick. The
     * bank flip lands on h==0's first tick, so only the pixel-0 read at
     * the end of h==799 still sees the fresh line under its write-side
     * label; a repeat line never flips and reads the held bank. */
    logic [9:0] rd_next;
    always_comb rd_next = x_shift
        ? {1'b0, 9'((h + 10'd1) >> 1)} : h + 10'd1;
    always_ff @(posedge clk) begin
        if (px_last) begin
            if (h == 10'd799)
                vid_mode_pix <= linebuf[flip_next ? wr_bank : !wr_bank][10'd0];
            else if (h < 10'd639)
                vid_mode_pix <= linebuf[!wr_bank][rd_next];
            else
                vid_mode_pix <= 16'h0000;
        end
    end
    /* Same nuance as the pixel path: during h==0 a pending flip's fresh
     * bank is still labeled write-side. Elsewhere the flip has landed. */
    always_comb vid_mode_filled =
        filled_q[(h == 10'd0 && flip_next) ? wr_bank : !wr_bank];

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
        S_IDLE, S_PROG, S_PROG_W, S_CFG, S_MODE, S_BLANK
    } state_t;
    state_t state /*verilator public_flat_rd*/;

    logic [15:0] attr /*verilator public_flat_rd*/;
    logic [15:0] config_ptr /*verilator public_flat_rd*/;

    /* Five fetched words hold any mode's config — mode 1's 16 bytes from
     * a halfword-aligned pointer span them all; the shifted view hands
     * each pipeline its bytes at fixed offsets. */
    logic [31:0] cfg[5];
    logic [2:0] cfg_i, cfg_c;
    logic [143:0] cfgw;
    always_comb cfgw = 144'({cfg[4], cfg[3], cfg[2], cfg[1], cfg[0]}
                            >> {config_ptr[1], 4'b0000});

    logic [9:0] px;
    logic [9:0] cw;
    always_comb cw = x_shift ? 10'd320 : 10'd640;

    /* The mode pipelines; the prog entry's mode bits pick one. */
    logic [2:0] mode_q;
    logic m3_start;
    logic m3_a_req;
    logic [13:0] m3_a_addr;
    logic m3_px_we;
    logic [9:0] m3_px_addr;
    logic [15:0] m3_px_data;
    logic m3_done, m3_filled;
    logic m1_start;
    logic m1_a_req;
    logic [13:0] m1_a_addr;
    logic m1_px_we;
    logic [9:0] m1_px_addr;
    logic [15:0] m1_px_data;
    logic m1_done, m1_filled;
    logic m2_start;
    logic m2_a_req;
    logic [13:0] m2_a_addr;
    logic m2_px_we;
    logic [9:0] m2_px_addr;
    logic [15:0] m2_px_data;
    logic m2_done, m2_filled;
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
        .vid_mode1_px_we(m1_px_we),
        .vid_mode1_px_addr(m1_px_addr),
        .vid_mode1_px_data(m1_px_data),
        .vid_mode1_done(m1_done),
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
        .a_gnt(a_gnt),
        .a_rdata(a_rdata),
        .vid_mode2_px_we(m2_px_we),
        .vid_mode2_px_addr(m2_px_addr),
        .vid_mode2_px_data(m2_px_data),
        .vid_mode2_done(m2_done),
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
        .vid_mode3_a_req(m3_a_req),
        .vid_mode3_a_addr(m3_a_addr),
        .a_gnt(a_gnt),
        .a_rdata(a_rdata),
        .vid_mode3_px_we(m3_px_we),
        .vid_mode3_px_addr(m3_px_addr),
        .vid_mode3_px_data(m3_px_data),
        .vid_mode3_done(m3_done),
        .vid_mode3_filled(m3_filled)
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
            sub_px_we = m1_px_we;
            sub_px_addr = m1_px_addr;
            sub_px_data = m1_px_data;
            sub_done = m1_done;
            sub_filled = m1_filled;
        end else if (mode_q == 3'd2) begin
            sub_a_req = m2_a_req;
            sub_a_addr = m2_a_addr;
            sub_px_we = m2_px_we;
            sub_px_addr = m2_px_addr;
            sub_px_data = m2_px_data;
            sub_done = m2_done;
            sub_filled = m2_filled;
        end else begin
            sub_a_req = m3_a_req;
            sub_a_addr = m3_a_addr;
            sub_px_we = m3_px_we;
            sub_px_addr = m3_px_addr;
            sub_px_data = m3_px_data;
            sub_done = m3_done;
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

    /* The write bank: the pipeline's pixels, the plane's own blank, or
     * the sprite stage painting after the fill went idle. */
    always_ff @(posedge clk) begin
        if (state == S_MODE && sub_px_we)
            linebuf[wr_bank][sub_px_addr] <= sub_px_data;
        else if (state == S_BLANK)
            linebuf[wr_bank][px] <= 16'h0000;
        else if (sp_we)
            linebuf[wr_bank][sp_addr] <= sp_data;
    end

    logic gnt_d;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wr_bank <= 1'b0;
            filled_q[0] <= 1'b0;
            filled_q[1] <= 1'b0;
            flip_next <= 1'b0;
            state <= S_IDLE;
            t <= '0;
            attr <= '0;
            config_ptr <= '0;
            for (int i = 0; i < 5; i++)
                cfg[i] <= '0;
            cfg_i <= '0;
            cfg_c <= '0;
            px <= '0;
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
            if (h == 10'd799 && state != S_IDLE)
                $fatal(1, "vid_mode underrun");
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
                             * pipeline is not built yet. */
                            state <= S_BLANK;
                            px <= '0;
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
                            cfg[cfg_c] <= a_rdata;
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
                    S_BLANK: begin
                        px <= px + 10'd1;
                        if (px == cw - 10'd1)
                            state <= S_IDLE;
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
