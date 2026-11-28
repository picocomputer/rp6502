/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's pixel stream to sys/'s video input, each canvas at its
 * own size: a 320x240 canvas is 320 pixels on 240 lines, and the scaler,
 * the shaders and the scanline filters of MiSTer do the rest. CLK_VIDEO
 * is clk_sys, so nothing crosses a clock.
 *
 * Every frame is 840000 clocks, the machine's 60 Hz, in either raster:
 *
 *  - 640 wide: 1600 clocks by 525 lines, CE_PIXEL every 2nd clock.
 *  - 320 wide: 262 lines of 3206 clocks, the last one 3234, CE_PIXEL on
 *    every 8th clock of a line. That is 240p at 15.7 kHz; no 262 equal
 *    lines make 840000, so the one long line is in the back porch.
 *
 * A line is active, front porch, HS, back porch. The picture is on lines
 * 0 to h-1 of the 640 raster and 1 to h of the 320 one, and VS sits in
 * the middle of what is left, so the picture is centred on a CRT.
 *
 * sys/ measures the raster and Main resyncs HDMI when what it measures
 * changes, so all but the colour is a function of the counters and the
 * canvas only, and the canvas is taken only at the end of a frame. A
 * program that selects the canvas it already has changes nothing but
 * the colour. The raster never stops: sync runs while the machine is
 * held in reset or has no firmware. vid_frame only sets its phase, and
 * since both rasters are as long as the machine's frame, it finds the
 * raster already there on every frame but the first.
 *
 * The machine sends pixel i of the screen's line Y on the clock 2i + 2
 * after that line starts, and vid_frame on the first clock of line 0.
 * The raster runs 3 clocks behind. A 640 wide line is read out of the
 * line buffer just behind its writer. A 320 wide canvas sends row k on
 * line 2k + 1, a row every 3200 clocks, and it is shown on line k + 1 of
 * the 320 raster, a row every 3206: the buffer's two halves take the
 * rows in turn, and a row is read at least 1613 clocks after it is
 * written and at least 1439 before the row after next overwrites it.
 *
 * The picture is black unless a frame started in the last frame period.
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

    /* The writer goes round the buffer, which a frame starts over. A
     * 640 wide row fills it and a 320 wide one half of it. Bit 5, alpha,
     * is not kept. */
    logic [14:0] line_buf[640];
    logic [9:0] wp;
    initial wp = '0;
    always_ff @(posedge clk_sys)
        if (vid_frame)
            wp <= '0;
        else if (vid_de) begin
            line_buf[wp] <= {vid_pixel[15:6], vid_pixel[4:0]};
            wp <= wp == 10'd639 ? '0 : wp + 10'd1;
        end

    /* h counts clocks and v lines. The last clock of a frame takes the
     * canvas, which the machine changed a line earlier, and decides
     * whether the next frame is shown. vid_frame puts the raster two
     * clocks before the end of a frame, so a new canvas starts a whole
     * frame. */
    logic [11:0] h;
    logic [9:0] v;
    logic [2:0] cv;
    logic seen, live;
    logic dbl, last_h, last;
    always_comb begin
        dbl = canvas_w(cv) == 10'd320;
        last_h = h == (dbl ? (v == 10'd261 ? 12'd3233 : 12'd3205)
                           : 12'd1599);
        last = last_h && v == (dbl ? 10'd261 : 10'd524);
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
            h <= dbl ? 12'd3232 : 12'd1598;
            v <= dbl ? 10'd261 : 10'd524;
        end else if (last_h) begin
            h <= '0;
            v <= last ? '0 : v + 10'd1;
        end else
            h <= h + 12'd1;
        if (vid_frame)
            seen <= 1'b1;
        else if (last) begin
            cv <= vid_canvas;
            live <= seen;
            seen <= 1'b0;
        end
    end

    /* A pixel is the clocks up to its strobe. The 320 raster's row k is
     * on line k + 1, its even rows in the buffer's first half. */
    logic pix, de_px, hs_px, vs_px;
    logic [9:0] x, ch, vs_top;
    always_comb begin
        ch = canvas_h(cv);
        pix = dbl ? &h[2:0] : h[0];
        x = dbl ? 10'(h[11:3]) : h[10:1];
        de_px = x < canvas_w(cv) && v - 10'(dbl) < ch;
        hs_px = dbl ? x >= 10'd330 && x < 10'd360
                    : x >= 10'd656 && x < 10'd752;
        vs_top = (dbl ? 10'd124 : 10'd250) + (ch >> 1);
        vs_px = v >= vs_top && v < vs_top + (dbl ? 10'd3 : 10'd2);
    end

    /* The read is on every clock and the outputs are set on the strobe,
     * to show until the next one. */
    logic [14:0] rd_q;
    always_ff @(posedge clk_sys)
        rd_q <= line_buf[dbl && !v[0] ? x + 10'd320 : x];

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
        mister_video_ce <= pix;
        if (pix) begin
            mister_video_de <= de_px;
            mister_video_hs <= hs_px;
            mister_video_vs <= vs_px;
            mister_video_r <= live && de_px ? {rd_q[4:0], rd_q[4:2]} : '0;
            mister_video_g <= live && de_px ? {rd_q[9:5], rd_q[9:7]} : '0;
            mister_video_b <= live && de_px
                ? {rd_q[14:10], rd_q[14:12]} : '0;
        end
        /* The aspect follows the canvas from the frame's first clock. */
        unique case (ch)
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

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_mister_video;
    always_comb unused_mister_video = vid_pixel[5];
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
