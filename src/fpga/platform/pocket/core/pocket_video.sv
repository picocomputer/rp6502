/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's picture handed to the Pocket's scaler. No CRT and no
 * beam is modelled: the raster is 800x525 CLOCKS at 25.2 MHz, and each
 * canvas row is one hs, its own pixels back to back, then porch — so a
 * 320-wide row spans two 800-clock slots and the shorter canvases come
 * out at their own heights.
 *
 * Analogue's filters, and the CRT mode that documents duplicated pixels
 * breaking it, work because no duplicated pixel ever arrives.
 *
 * The machine emits every canvas pixel exactly once — its de IS the
 * canvas — so this stage is only clock alignment: pixels arrive at one
 * per two clk_sys and leave at one per clk_vid, the same rate, and the
 * FIFO buffers jitter, not content. Both clocks come off the same PLL;
 * the reader's raster starts on the first crossed frame pulse and
 * never resynchronizes, and its window trails the writer by a few
 * pixels so the writer is always ahead.
 */

module pocket_video (
    /* The machine's domain. */
    input logic clk_sys,
    input logic rst_n,
    input logic [15:0] vid_pixel,
    input logic vid_de,
    input logic vid_frame,
    /* The machine's canvas, vga.h's encoding, latched by the machine at
     * vblank; the reader crosses it and mirrors the machine's own row
     * schedule with it. */
    input logic [2:0] vid_canvas,

    /* The scaler's domain. */
    input logic clk_vid,
    input logic vrst_n,
    output logic [23:0] pocket_video_rgb,
    output logic pocket_video_de,
    output logic pocket_video_skip,
    output logic pocket_video_vs,
    output logic pocket_video_hs
);

    localparam int H_TOTAL = 800;
    localparam int V_TOTAL = 525;
    localparam int V_ACTIVE = 480;
    /* Pixels right after hs, one small fixed offset for every width. */
    localparam int X_DE0 = 8;

    /* vga.h's vga_canvas_t. Console (0) and 640x480 (3) decode alike. */
    localparam logic [2:0] CV_320_240 = 3'd1;
    localparam logic [2:0] CV_320_180 = 3'd2;
    localparam logic [2:0] CV_640_360 = 3'd4;

    function automatic logic cv_is_x2(input logic [2:0] cv);
        cv_is_x2 = cv == CV_320_240 || cv == CV_320_180;
    endfunction

    /* Which raster lines carry rows — the machine's own schedule: canvas
     * rows on the first line of each doubled pair, the 16:9 heights
     * between lines 60 and 420, nothing anywhere else. */
    function automatic logic cv_row_sel(input logic [2:0] cv,
                                        input logic [9:0] line);
        cv_row_sel = line < 10'(V_ACTIVE);
        if (cv == CV_320_180 || cv == CV_640_360)
            cv_row_sel = line >= 10'd60 && line < 10'd420;
        if (cv_is_x2(cv))
            cv_row_sel = cv_row_sel && !line[0];
    endfunction

    /* The frame pulse crosses as a toggle. */
    logic frame_t;
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n)
            frame_t <= 1'b0;
        else if (vid_frame)
            frame_t <= !frame_t;
    end
    logic frame_t1, frame_t2, frame_t3;
    always_ff @(posedge clk_vid or negedge vrst_n) begin
        if (!vrst_n) begin
            frame_t1 <= 1'b0;
            frame_t2 <= 1'b0;
            frame_t3 <= 1'b0;
        end else begin
            frame_t1 <= frame_t;
            frame_t2 <= frame_t1;
            frame_t3 <= frame_t2;
        end
    end
    logic frame_pulse;
    always_comb frame_pulse = frame_t2 != frame_t3;

    /* Canvas pixels cross through the FIFO — jitter-deep, because the
     * two sides run at the same rate. */
    logic fifo_empty, fifo_full;
    logic [15:0] fifo_pixel;
    logic take;
    pocket_fifo #(
        .WIDTH(16),
        .DEPTH_LOG2(4)
    ) fifo (
        .wclk(clk_sys),
        .wrst_n(rst_n),
        .w_stb(vid_de),
        .w_data(vid_pixel),
        .pocket_fifo_full(fifo_full),
        .rclk(clk_vid),
        .rrst_n(vrst_n),
        .r_take(take),
        .pocket_fifo_empty(fifo_empty),
        .pocket_fifo_rdata(fifo_pixel)
    );

`ifdef VERILATOR
    always_ff @(posedge clk_sys) begin
        if (vid_de && fifo_full)
            $error("pocket_video: pixel fifo overflow");
    end
    always_ff @(posedge clk_vid) begin
        if (take && fifo_empty)
            $error("pocket_video: pixel fifo underflow");
    end
`endif

    /* The reader's raster: armed by the first frame pulse, then free
     * running at the writer's exact rate. */
    logic running;
    logic [9:0] x;
    logic [9:0] y;

    /* The reader's copy of the canvas, crossed and held for a whole
     * frame so the window cannot tear mid-picture. */
    logic [2:0] canvas_s1, canvas_s2, canvas;
    always_ff @(posedge clk_vid or negedge vrst_n) begin
        if (!vrst_n) begin
            canvas_s1 <= '0;
            canvas_s2 <= '0;
            canvas <= '0;
        end else begin
            canvas_s1 <= vid_canvas;
            canvas_s2 <= canvas_s1;
            if (!running || (x == 10'(H_TOTAL - 1) && y == 10'(V_TOTAL - 1)))
                canvas <= canvas_s2;
        end
    end

    logic [9:0] cw;   /* the canvas's width: the de run and the pops */
    logic [2:0] slot; /* video.json's scaler_modes, same order */
    always_comb begin
        cw = cv_is_x2(canvas) ? 10'd320 : 10'd640;
        unique case (canvas)
            CV_640_360: slot = 3'd1;
            CV_320_240: slot = 3'd2;
            CV_320_180: slot = 3'd3;
            default: slot = 3'd0; /* console and 640x480 */
        endcase
    end

    /* de is the canvas, in the same slot the machine sends it: its own
     * width, back to back, one pop per asserted cycle. */
    logic de_sel;
    always_comb de_sel = running
        && x >= 10'(X_DE0) && x < 10'(X_DE0) + cw
        && cv_row_sel(canvas, y);
    always_comb take = de_sel;

    /* The end-of-line word: the cycle after de falls on every row, rgb
     * names the scaler slot — endline[23:13] the parameter, [2:0] the
     * Set Scaler Slot function code, zeros. Zeros anywhere else are
     * safe: the scaler samples this position only. */
    logic endline_now;
    always_comb endline_now = running && cv_row_sel(canvas, y)
        && x == 10'(X_DE0) + cw;

    logic [7:0] r8, g8, b8;
    always_comb begin
        r8 = {fifo_pixel[4:0], fifo_pixel[4:2]};
        g8 = {fifo_pixel[10:6], fifo_pixel[10:8]};
        b8 = {fifo_pixel[15:11], fifo_pixel[15:13]};
    end

    always_ff @(posedge clk_vid or negedge vrst_n) begin
        if (!vrst_n) begin
            running <= 1'b0;
            x <= '0;
            y <= '0;
            pocket_video_rgb <= '0;
            pocket_video_de <= 1'b0;
            pocket_video_vs <= 1'b0;
            pocket_video_hs <= 1'b0;
        end else begin
            if (!running) begin
                if (frame_pulse) begin
                    running <= 1'b1;
                    x <= '0;
                    y <= '0;
                end
            end else if (x == 10'(H_TOTAL - 1)) begin
                x <= '0;
                y <= y == 10'(V_TOTAL - 1) ? '0 : y + 10'd1;
            end else begin
                x <= x + 10'd1;
            end

            pocket_video_vs <= (running && x == 10'(H_TOTAL - 1)
                                && y == 10'(V_TOTAL - 1))
                || (!running && frame_pulse);
            /* One hs per row period: every 800-clock slot at full width,
             * every second one when a row's period spans two — so the
             * order on the wire is always hs, pixels, porch. */
            pocket_video_hs <= running && x == 10'd2
                && (!cv_is_x2(canvas) || !y[0]);
            pocket_video_de <= de_sel;
            pocket_video_rgb <= de_sel ? {r8, g8, b8}
                : endline_now ? {11'(slot), 13'd0}
                : 24'h0;
        end
    end

    /* Nothing is skipped: only canvas pixels ever arrive. */
    always_comb pocket_video_skip = 1'b0;

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_video;
    always_comb unused_pocket_video = fifo_pixel[5]; /* the alpha bit */
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
