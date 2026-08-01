/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's beam handed to the Pocket's scaler. The machine paints
 * at 50.4 MHz, one pixel strobe per two clocks; the scaler wants a
 * 25.2 MHz raster with its own manners — one vsync pulse at the
 * origin, one hsync pulse at x==3 on every line, data enable across
 * the active window, black outside it. Both rasters come off the same
 * PLL, so a shallow FIFO and a frame pulse are the whole alignment:
 * the reader's raster starts on the first crossed frame pulse and
 * never resynchronizes, the de window sits a few clocks past the
 * origin so the writer stays ahead, and occupancy holds still from
 * then on — moving occupancy is a broken clock family, and the
 * simulation asserts it never moves to the rails.
 *
 * The scaler is told the truth about the canvas. The machine paints
 * every canvas into one 640x480 beam — 320-wide canvases as doubled
 * pixels, 240- and 180-line canvases as doubled lines, the 16:9
 * heights under a 60-line letterbox — and this stage undoes exactly
 * that presentation: skip marks the duplicate of each doubled pixel
 * (the scaler latches only the first, and Analogue's CRT mode
 * requires no horizontal duplication), de simply never asserts on
 * repeated or letterbox lines, and the end-of-line word after each
 * active line names the scaler slot for the canvas, so the Pocket
 * scales the native picture with its own filters. The reader still
 * pops every pixel of the full window whatever the canvas — the
 * FIFO's flow is the clock alignment and does not change with the
 * picture.
 */

module pocket_video (
    /* The machine's domain. */
    input logic clk_sys,
    input logic rst_n,
    input logic [15:0] vid_pixel,
    input logic vid_de,
    input logic vid_frame,
    /* The machine's canvas, vga.h's encoding. Latched by the machine at
     * vblank, so it is quasi-static here; sampled once per frame below. */
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
    localparam int X_DE0 = 8;
    localparam int H_ACTIVE = 640;
    localparam int V_ACTIVE = 480;

    /* vga.h's vga_canvas_t, and the scaler slot each one maps to —
     * video.json's scaler_modes array in the same order. */
    /* Console (0) and 640x480 (3) both decode to the default arm below,
     * so only the canvases that change the geometry are named. */
    localparam logic [2:0] CV_320_240 = 3'd1;
    localparam logic [2:0] CV_320_180 = 3'd2;
    localparam logic [2:0] CV_640_360 = 3'd4;

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

    /* Active pixels cross through the FIFO. */
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
    logic de_now;
    always_comb de_now = running
        && x >= 10'(X_DE0) && x < 10'(X_DE0 + H_ACTIVE)
        && y < 10'(V_ACTIVE);
    always_comb take = de_now;

    /* The canvas, crossed and then held for a whole frame so the
     * geometry cannot tear mid-picture. A change lands during vblank on
     * the machine side and is sampled here at the next origin; the one
     * frame in between is the mode-switch frame the scaler is already
     * discarding, since the slot request also takes effect a frame
     * late. */
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

    /* What the canvas means to this raster: which beam pixels are the
     * canvas's own and which are the presentation. */
    logic cv_x2;      /* 320 wide: every beam pixel pair is one canvas pixel */
    logic cv_y2;      /* 240/180 tall: every beam line pair is one canvas row */
    logic cv_box;     /* 16:9: the picture sits under a 60-line letterbox */
    logic [2:0] slot;
    always_comb begin
        cv_x2 = canvas == CV_320_240 || canvas == CV_320_180;
        cv_y2 = canvas == CV_320_240 || canvas == CV_320_180;
        cv_box = canvas == CV_320_180 || canvas == CV_640_360;
        unique case (canvas)
            CV_640_360: slot = 3'd1;
            CV_320_240: slot = 3'd2;
            CV_320_180: slot = 3'd3;
            default: slot = 3'd0; /* console and 640x480 */
        endcase
    end

    /* A line carries canvas rows when it is inside the letterbox and is
     * the first of its doubled pair. The letterbox top is 60 even, so
     * the pair parity is y[0] either way. */
    logic line_sel;
    always_comb begin
        line_sel = y < 10'(V_ACTIVE);
        if (cv_box)
            line_sel = y >= 10'd60 && y < 10'd420;
        if (cv_y2)
            line_sel = line_sel && !y[0];
    end

    /* The de and skip the scaler sees: de only on canvas rows, skip on
     * the duplicate of each doubled pixel. X_DE0 is even, so the first
     * of a pair lands on even x. */
    logic de_sel, skip_sel;
    always_comb de_sel = de_now && line_sel;
    always_comb skip_sel = de_sel && cv_x2 && x[0];

    /* The end-of-line word: the cycle after de falls on every line that
     * carried it, rgb names the scaler slot — endline[23:13] the
     * parameter, [2:0] the Set Scaler Slot function code, zeros. Zeros
     * anywhere else are safe: the scaler samples this position only. */
    logic endline_now;
    always_comb endline_now = running && line_sel
        && x == 10'(X_DE0 + H_ACTIVE);

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
            pocket_video_skip <= 1'b0;
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
            pocket_video_hs <= running && x == 10'd2;
            pocket_video_de <= de_sel;
            pocket_video_skip <= skip_sel;
            pocket_video_rgb <= de_sel ? {r8, g8, b8}
                : endline_now ? {11'(slot), 13'd0}
                : 24'h0;
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_video;
    always_comb unused_pocket_video = fifo_pixel[5]; /* the alpha bit */
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
