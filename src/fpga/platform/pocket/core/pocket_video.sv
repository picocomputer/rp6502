/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's beam handed to the Pocket's scaler. The machine paints
 * at 100.8 MHz, one pixel strobe per four clocks; the scaler wants a
 * 25.2 MHz raster with its own manners — one vsync pulse at the
 * origin, one hsync pulse at x==3 on every line, data enable across
 * the active window, black outside it. Both rasters come off the same
 * PLL, so a shallow FIFO and a frame pulse are the whole alignment:
 * the reader's raster starts on the first crossed frame pulse and
 * never resynchronizes, the de window sits a few clocks past the
 * origin so the writer stays ahead, and occupancy holds still from
 * then on — moving occupancy is a broken clock family, and the
 * simulation asserts it never moves to the rails.
 */

module pocket_video (
    /* The machine's domain. */
    input logic clk_sys,
    input logic rst_n,
    input logic [15:0] vid_pixel,
    input logic vid_de,
    input logic vid_frame,

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
            pocket_video_hs <= running && x == 10'd2;
            pocket_video_de <= de_now;
            pocket_video_rgb <= de_now ? {r8, g8, b8} : 24'h0;
        end
    end

    always_comb pocket_video_skip = 1'b0;

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_video;
    always_comb unused_pocket_video = fifo_pixel[5]; /* the alpha bit */
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
