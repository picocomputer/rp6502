/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine sends each canvas pixel once, one every two clk_mach
 * cycles along a row, on an 800 by 525 raster. clk_mach is clk_sys
 * through the savestate gate, and clk_sys and clk_vid come off one PLL
 * at 50.4 and 25.2 MHz. The reader, which is the FIFO's read side, is
 * clocked by clk_vid and runs a raster of the same size at the same
 * rate, so the FIFO only aligns the two in phase.
 *
 * The writer, which is the FIFO's write side, is clocked by clk_mach
 * and not by clk_sys, because vid_de can be left high when the gate
 * stops clk_mach, and a writer on clk_sys would then push the same
 * pixel on every clk_sys edge.
 *
 * A savestate stops clk_mach mid-frame while clk_vid keeps running, so
 * when clk_mach returns the reader's raster no longer lines up with the
 * machine's and the reader has to find the start of a frame again.
 */

module pocket_video (
    input logic clk_mach,
    input logic [15:0] vid_pixel,
    input logic vid_de,
    input logic vid_frame,
    input logic [2:0] vid_canvas,
    input logic run,

    input logic clk_vid,
    output logic [23:0] pocket_video_rgb,
    output logic pocket_video_de,
    output logic pocket_video_skip,
    output logic pocket_video_vs,
    output logic pocket_video_hs
);

    localparam int H_TOTAL = 800;
    localparam int V_TOTAL = 525;
    localparam int V_ACTIVE = 480;
    /* X_DE0 also sets how far the reader trails the writer. relock
     * starts the raster at x = 0 when the frame's first pixel is at the
     * head of the FIFO, and the first pop is at x = X_DE0, so the writer
     * stays at least X_DE0 pixels ahead until locked drops. */
    localparam int X_DE0 = 8;

    /* These are vga.h's vga_canvas_t values. The console (0) and
     * 640x480 (3) canvases decode alike. */
    localparam logic [2:0] CV_320_240 = 3'd1;
    localparam logic [2:0] CV_320_180 = 3'd2;
    localparam logic [2:0] CV_640_360 = 3'd4;

    function automatic logic cv_w320(input logic [2:0] cv);
        cv_w320 = cv == CV_320_240 || cv == CV_320_180;
    endfunction

    function automatic logic [9:0] cv_ch(input logic [2:0] cv);
        cv_ch = cv == CV_320_240 ? 10'd240
            : cv == CV_320_180 ? 10'd180
            : cv == CV_640_360 ? 10'd360 : 10'(V_ACTIVE);
    endfunction
    /* A 320 wide canvas spans two lines of timing per row of graphics, so
     * the writer hands one row over every other line and the reader has to
     * take them at the same cadence or it outruns the FIFO. The scaler still
     * receives cv_ch rows either way. */
    function automatic logic cv_row_sel(input logic [2:0] cv,
                                        input logic [9:0] line);
        logic [9:0] lines;
        lines = cv_w320(cv) ? 10'(cv_ch(cv) << 1) : cv_ch(cv);
        cv_row_sel = cv_w320(cv) ? !line[0] && line < lines
                                 : line < lines;
    endfunction

    /* sof_arm tags the frame's first pixel. vid_frame is high only on a
     * frame's first clock. compose registers de, so on that clock vid_de
     * holds de from the last clock of line 524, which is in blanking, and
     * vid_frame and vid_de are never high together. */
    logic sof_arm;
    initial sof_arm = 1'b0;
    always_ff @(posedge clk_mach) begin
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
        .wclk(clk_mach),
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

    (* preserve *) logic run_v1, run_v2;
    initial begin
        run_v1 = 1'b1;
        run_v2 = 1'b1;
    end
    always_ff @(posedge clk_vid) begin
        run_v1 <= run;
        run_v2 <= run_v1;
    end

`ifdef VERILATOR
    logic checked = 1'b0;
    always_ff @(posedge clk_mach)
        if (!run)
            checked <= 1'b0;
        else if (vid_frame)
            checked <= 1'b1;
    always_ff @(posedge clk_mach) begin
        if (checked && run && vid_de && fifo_full)
            $error("pocket_video: pixel fifo overflow");
    end
    always_ff @(posedge clk_vid) begin
        if (checked && run_v2 && locked && take && fifo_empty)
            $error("pocket_video: pixel fifo underflow at y=%0d x=%0d", y, x);
    end
`endif

    /* canvas is also loaded on every clk_vid edge while unlocked,
     * because if the reader kept an older canvas than the writer's, it
     * could pop a different number of pixels than were pushed in its
     * first locked frame. vid_canvas changes only at the start of the
     * machine's line 524, so canvas_s2 has settled before canvas is
     * loaded from it at relock or at the end of the reader's frame. */
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
        cw = cv_w320(canvas) ? 10'd320 : 10'd640;
        unique case (canvas)
            CV_640_360: slot = 3'd1;
            CV_320_240: slot = 3'd2;
            CV_320_180: slot = 3'd3;
            default: slot = 3'd0;
        endcase
    end

    logic de_sel;
    always_comb de_sel = raster
        && x >= 10'(X_DE0) && x < 10'(X_DE0) + cw
        && cv_row_sel(canvas, y);
    /* While run_v2 is low the tagged pixel is discarded too, because a
     * tag still in the FIFO when locked drops would otherwise stay at
     * the head, and relock would restart the reader's raster on a frame
     * start that the writer passed before clk_mach stopped. */
    always_comb take = locked ? de_sel
        : (!fifo_empty && (!run_v2 || !fifo_sof));

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
     * hands a row over every other line of timing; without this the rows
     * land on every second row of the scaler's frame. */
    logic hs_gap;
    always_comb hs_gap = cv_w320(canvas) && y[0]
        && y < 10'(cv_ch(canvas) << 1);

    logic relock;
    always_comb relock = run_v2 && !locked && !fifo_empty && fifo_sof;

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
        if (relock) begin
            raster <= 1'b1;
            locked <= 1'b1;
            x <= '0;
            y <= '0;
        end else begin
            if (!run_v2)
                locked <= 1'b0;
            if (raster) begin
                if (x == 10'(H_TOTAL - 1)) begin
                    x <= '0;
                    y <= y == 10'(V_TOTAL - 1) ? '0 : y + 10'd1;
                end else begin
                    x <= x + 10'd1;
                end
            end
        end

        pocket_video_vs <= relock || (raster && x == 10'(H_TOTAL - 1)
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
