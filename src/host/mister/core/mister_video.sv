/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's pixel stream to sys/'s video input. CLK_VIDEO is clk_sys
 * and CE_PIXEL comes every other clock, so nothing crosses a clock.
 *
 * The output raster is 640x480 at 60 Hz, 1600 clocks by 525 lines, and
 * it never stops: sync runs while the machine is held in reset or has no
 * firmware. vid_frame only sets its phase. The machine sends pixel i of
 * the screen's line Y on the clock 2i + 2 after that line starts, and
 * vid_frame on the first clock of line 0. The raster runs 3 clocks
 * behind, the least that has each pixel written a clock before it is
 * read, so a line is read out of the one line buffer just behind its
 * writer. The phase is the same on every frame, so the raster is set
 * once and from then on vid_frame finds it already there.
 *
 * A 320 wide canvas sends a row on every odd line, as wiring.sv says, so
 * row k shows on lines 2k + 1 and 2k + 2, each pixel twice. The picture
 * is black unless a frame started in the last frame period.
 */

module mister_video
    import canvas_pkg::*;
(
    input logic clk_sys,
    input logic [15:0] vid_pixel,
    input logic vid_de,
    input logic vid_frame,
    input logic [2:0] vid_canvas,

    output logic mister_video_ce,
    output logic [7:0] mister_video_r,
    output logic [7:0] mister_video_g,
    output logic [7:0] mister_video_b,
    output logic mister_video_hs,
    output logic mister_video_vs,
    output logic mister_video_de,
    output logic [12:0] mister_video_arx,
    output logic [12:0] mister_video_ary
);

    /* The writer counts a line's pixels, which come every other clock,
     * and starts over after two clocks with none, which only the gap
     * between lines has. Bit 5, alpha, is not kept. */
    logic [14:0] line_buf[640];
    logic [9:0] wr_col;
    logic wr_de_q;
    initial begin
        wr_col = '0;
        wr_de_q = 1'b0;
    end
    always_ff @(posedge clk_sys) begin
        wr_de_q <= vid_de;
        if (vid_de) begin
            line_buf[wr_col] <= {vid_pixel[15:6], vid_pixel[4:0]};
            wr_col <= wr_col + 10'd1;
        end else if (!wr_de_q)
            wr_col <= '0;
    end

    /* h counts clocks and v lines. The last clock of a frame takes the
     * canvas, which the machine changed a line earlier, and decides
     * whether the next frame is shown. */
    logic [10:0] h;
    logic [9:0] v;
    logic [2:0] cv;
    logic seen, live;
    logic last_h, last;
    always_comb begin
        last_h = h == 11'd1599;
        last = last_h && v == 10'd524;
    end
    initial begin
        h = '0;
        v = '0;
        cv = '0;
        seen = 1'b0;
        live = 1'b0;
    end
    always_ff @(posedge clk_sys) begin
        if (vid_frame) begin
            h <= 11'd1598;
            v <= 10'd524;
        end else if (last_h) begin
            h <= '0;
            v <= v == 10'd524 ? '0 : v + 10'd1;
        end else
            h <= h + 11'd1;
        if (vid_frame)
            seen <= 1'b1;
        else if (last) begin
            cv <= vid_canvas;
            live <= seen;
            seen <= 1'b0;
        end
    end

    logic dbl;
    logic [9:0] x, rows;
    always_comb begin
        dbl = canvas_w(cv) == 10'd320;
        rows = dbl ? 10'(canvas_h(cv) << 1) : canvas_h(cv);
        x = h[10:1];
    end

    /* The read is on pixel x's even clock and the outputs are set on its
     * odd one, to show for the two clocks after. */
    logic [14:0] rd_q;
    always_ff @(posedge clk_sys)
        rd_q <= line_buf[dbl ? 10'(h[10:2]) : x];

    logic de_px;
    always_comb de_px = x < 10'd640 && v - 10'(dbl) < rows;

    initial begin
        mister_video_ce = 1'b0;
        mister_video_r = '0;
        mister_video_g = '0;
        mister_video_b = '0;
        mister_video_hs = 1'b0;
        mister_video_vs = 1'b0;
        mister_video_de = 1'b0;
        mister_video_arx = '0;
        mister_video_ary = '0;
    end
    always_ff @(posedge clk_sys) begin
        mister_video_ce <= h[0];
        if (h[0]) begin
            mister_video_de <= de_px;
            mister_video_hs <= x >= 10'd656 && x < 10'd752;
            mister_video_vs <= v == 10'd490 || v == 10'd491;
            mister_video_r <= live && de_px ? {rd_q[4:0], rd_q[4:2]} : '0;
            mister_video_g <= live && de_px ? {rd_q[9:5], rd_q[9:7]} : '0;
            mister_video_b <= live && de_px
                ? {rd_q[14:10], rd_q[14:12]} : '0;
            unique case (canvas_h(cv))
                10'd360, 10'd180: begin
                    mister_video_arx <= 13'd16;
                    mister_video_ary <= 13'd9;
                end
                10'd400, 10'd200: begin
                    mister_video_arx <= 13'd16;
                    mister_video_ary <= 13'd10;
                end
                default: begin
                    mister_video_arx <= 13'd4;
                    mister_video_ary <= 13'd3;
                end
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_mister_video;
    always_comb unused_mister_video = vid_pixel[5];
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
