/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine sends each canvas pixel once, one every two clk_sys
 * cycles along a row, on an 800 by 525 raster. clk_sys and clk_vid come
 * off one PLL at 50.4 and 25.2 MHz. The writer, which is the FIFO's
 * write side, is clocked by clk_sys. The reader, which is the FIFO's
 * read side, is clocked by clk_vid and runs a raster of the same size
 * at the same rate, so the FIFO only aligns the two in phase. The reader
 * finds the start of a frame once, and from then on the two rasters
 * stay in step.
 */

module pocket_video
    import canvas_pkg::*;
(
    input logic clk_sys,
    input logic [15:0] vid_pixel,
    input logic vid_de,
    input logic vid_frame,
    input logic [2:0] vid_canvas,

    input logic clk_vid,
    output logic [23:0] pocket_video_rgb,
    output logic pocket_video_de,
    output logic pocket_video_skip,
    output logic pocket_video_vs,
    output logic pocket_video_hs
);

    localparam int H_TOTAL = 800;
    localparam int V_TOTAL = 525;
    /* X_DE0 also sets how far the reader trails the writer. lock
     * starts the raster at x = 0 when the frame's first pixel is at the
     * head of the FIFO, and the first pop is at x = X_DE0, so the writer
     * stays at least X_DE0 pixels ahead. */
    localparam int X_DE0 = 8;

    /* A 320 wide canvas spans two lines of timing per row of graphics, so
     * the writer hands one row over on every odd line and the reader has to
     * take them at the same cadence or it outruns the FIFO. The scaler still
     * receives canvas_h rows either way. */
    function automatic logic cv_row_sel(input logic [2:0] cv,
                                        input logic [9:0] line);
        logic dbl;
        logic [9:0] lines;
        dbl = canvas_w(cv) == 10'd320;
        lines = dbl ? 10'(canvas_h(cv) << 1) : canvas_h(cv);
        cv_row_sel = dbl ? line[0] && line < lines
                         : line < lines;
    endfunction

    /* sof_arm tags the frame's first pixel. vid_frame is high only on the
     * first clock of the frame sent to the screen. compose registers de, so
     * on that clock vid_de holds de from the last clock of the screen's
     * line 524, which carries no pixels, and vid_frame and vid_de are never
     * high together. */
    logic sof_arm;
    initial sof_arm = 1'b0;
    always_ff @(posedge clk_sys) begin
        if (vid_frame)
            sof_arm <= 1'b1;
        else if (vid_de)
            sof_arm <= 1'b0;
    end

    logic fifo_empty, fifo_full;
    logic [16:0] fifo_head;
    logic [15:0] fifo_pixel;
    logic fifo_sof;
    logic take;
    always_comb begin
        fifo_sof = fifo_head[16];
        fifo_pixel = fifo_head[15:0];
    end
    pocket_fifo #(
        .WIDTH(17),
        .DEPTH_LOG2(4)
    ) fifo (
        .wclk(clk_sys),
        .w_stb(vid_de),
        .w_data({sof_arm, vid_pixel}),
        .pocket_fifo_full(fifo_full),
        .rclk(clk_vid),
        .r_take(take),
        .pocket_fifo_empty(fifo_empty),
        .pocket_fifo_rdata(fifo_head)
    );

    logic raster, locked /*verilator public_flat_rd*/;
    logic [9:0] x /*verilator public_flat_rd*/;
    logic [9:0] y /*verilator public_flat_rd*/;

`ifdef VERILATOR
    logic checked = 1'b0;
    always_ff @(posedge clk_sys)
        if (vid_frame)
            checked <= 1'b1;
    always_ff @(posedge clk_sys) begin
        if (checked && vid_de && fifo_full)
            $error("pocket_video: pixel fifo overflow");
    end
    always_ff @(posedge clk_vid) begin
        if (checked && locked && take && fifo_empty)
            $error("pocket_video: pixel fifo underflow at y=%0d x=%0d", y, x);
    end
`endif

    /* canvas is also loaded on every clk_vid edge while unlocked,
     * because if the reader kept an older canvas than the writer's, it
     * could pop a different number of pixels than were pushed in its
     * first locked frame. vid_canvas changes only as the machine's frame
     * starts, which is the start of the screen's line 524, so canvas_s2
     * has settled before canvas is loaded from it at lock or at the end of
     * the reader's frame. */
    (* preserve *) logic [2:0] canvas_s1, canvas_s2;
    logic [2:0] canvas;
    initial begin
        canvas_s1 = '0;
        canvas_s2 = '0;
        canvas = '0;
    end
    always_ff @(posedge clk_vid) begin
        canvas_s1 <= vid_canvas;
        canvas_s2 <= canvas_s1;
        if (!locked || (x == 10'(H_TOTAL - 1) && y == 10'(V_TOTAL - 1)))
            canvas <= canvas_s2;
    end

    logic [9:0] cw;
    logic [2:0] slot; /* This indexes video.json's scaler_modes. */
    always_comb begin
        cw = canvas_w(canvas);
        unique case (canvas)
            CV_640_360: slot = 3'd1;
            CV_320_240: slot = 3'd2;
            CV_320_180: slot = 3'd3;
            CV_320_200: slot = 3'd4;
            CV_640_400: slot = 3'd5;
            default: slot = 3'd0;
        endcase
    end

    logic de_sel;
    always_comb de_sel = raster
        && x >= 10'(X_DE0) && x < 10'(X_DE0) + cw
        && cv_row_sel(canvas, y);
    always_comb take = locked ? de_sel : (!fifo_empty && !fifo_sof);

    logic endline_now;
    always_comb endline_now = raster && cv_row_sel(canvas, y)
        && x == 10'(X_DE0) + cw;

    logic [7:0] r8, g8, b8;
    always_comb begin
        r8 = {fifo_pixel[4:0], fifo_pixel[4:2]};
        g8 = {fifo_pixel[10:6], fifo_pixel[10:8]};
        b8 = {fifo_pixel[15:11], fifo_pixel[15:13]};
    end

    /* HS delimits a scanline and the scaler counts rows by it, so a line
     * that carries no scanline must carry no HS either. A 320 wide canvas
     * hands a row over on every odd line of timing; without this the rows
     * land on every second row of the scaler's frame. */
    logic hs_gap;
    always_comb hs_gap = cw == 10'd320 && !y[0]
        && y < 10'(canvas_h(canvas) << 1);

    logic lock;
    always_comb lock = !locked && !fifo_empty && fifo_sof;

    initial begin
        raster = 1'b0;
        locked = 1'b0;
        x = '0;
        y = '0;
        pocket_video_rgb = '0;
        pocket_video_de = 1'b0;
        pocket_video_vs = 1'b0;
        pocket_video_hs = 1'b0;
    end
    always_ff @(posedge clk_vid) begin
        if (lock) begin
            raster <= 1'b1;
            locked <= 1'b1;
            x <= '0;
            /* The tagged pixel starts the frame's first row, which a 320
             * wide canvas sends on line 1. */
            y <= cw == 10'd320 ? 10'd1 : 10'd0;
        end else begin
            if (raster) begin
                if (x == 10'(H_TOTAL - 1)) begin
                    x <= '0;
                    y <= y == 10'(V_TOTAL - 1) ? '0 : y + 10'd1;
                end else begin
                    x <= x + 10'd1;
                end
            end
        end

        pocket_video_vs <= lock || (raster && x == 10'(H_TOTAL - 1)
                                      && y == 10'(V_TOTAL - 1));
        pocket_video_hs <= raster && x == 10'd2 && !hs_gap;
        pocket_video_de <= de_sel;
        pocket_video_rgb <= de_sel ? (locked ? {r8, g8, b8} : 24'h0)
            : endline_now ? {11'(slot), 13'd0}
            : 24'h0;
    end

    always_comb pocket_video_skip = 1'b0;

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_video;
    always_comb unused_pocket_video = fifo_pixel[5]; /* Bit 5 is alpha. */
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
