/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's picture handed to the Pocket's scaler. No CRT and no
 * beam is modelled: the raster is 800x525 CLOCKS at 25.2 MHz, one row
 * a slot — the canvas's rows on the first canvas-height slots, each
 * its own pixels back to back, then porch. The scaler mode names the
 * shape and the scaler does the rest.
 *
 * Analogue's filters, and the CRT mode that documents duplicated pixels
 * breaking it, work because no duplicated pixel ever arrives.
 *
 * The machine emits every canvas pixel exactly once — its de IS the
 * canvas — so this stage is only clock alignment: pixels arrive at one
 * per two clk_mach and leave at one per clk_vid, the same rate, and the
 * FIFO buffers jitter, not content. Both clocks come off the same PLL,
 * and the reader's window trails the writer by a few pixels so the
 * writer is always ahead.
 *
 * The writer's side is clocked by clk_mach and not by the clock behind
 * the gate. de is a level the machine drives, and a savestate takes the
 * machine's clock away wherever it happens to be: a de frozen high
 * would push the same pixel into the FIFO on every edge of a clock that
 * did not stop. Clocked by the gate's output it simply stops, which is
 * what it means.
 *
 * That phase lock is the whole design, and a savestate breaks it: the
 * machine's beam stops mid-frame — or comes back from a blob at a
 * different mid-frame — while this reader's clock never stops. A reader
 * that free-ran through it would come back popping at times unrelated
 * to the writer's pushes and would tear on every line thereafter; that
 * was the scrambled picture every savestate left behind on hardware.
 *
 * So the lock is dropped and taken again — but the raster is not. The
 * scaler is a separate machine and it is not asleep: it wants a frame
 * every sixteen milliseconds and a savestate is tens of those. The
 * reader keeps counting, keeps its syncs, keeps its de and sends black
 * until it can line up again. Taking the lock costs one long frame;
 * going quiet for three costs the scaler its lock.
 *
 * Lining up is done by the pixels and not by a crossing. The frame's
 * first pixel is tagged as it is pushed and the reader waits for that
 * tag to arrive at the head of the queue, discarding what is in front
 * of it. A crossed frame pulse cannot do this job: it arrives two or
 * three of the reader's clocks after the writer's frame began, by which
 * time the frame's first pixels are already in the queue and anything
 * that was clearing it has eaten them. The tag has no such gap — it is
 * in the queue, in front of the pixel it belongs to.
 */

module pocket_video (
    /* The machine's domain, on the machine's clock: the one the
     * savestate gate stops. */
    input logic clk_mach,
    input logic [15:0] vid_pixel,
    input logic vid_de,
    input logic vid_frame,
    /* The machine's canvas, vga.h's encoding, latched by the machine at
     * vblank; the reader crosses it and mirrors the machine's own row
     * schedule with it. */
    input logic [2:0] vid_canvas,
    /* Whether the machine has its clock. A savestate stops the beam
     * mid-frame and the picture legitimately freezes; this is what
     * tells the reader that the pixels have stopped coming rather than
     * that it has fallen behind. */
    input logic run,

    /* The scaler's domain. */
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
    /* Pixels right after hs, one small fixed offset for every width.
     * It is also the trail: the reader takes the lock the moment the
     * frame's first pixel reaches the head of the queue and then waits
     * this many of its own clocks before popping it, so the writer is
     * that far ahead for the rest of the frame. */
    localparam int X_DE0 = 8;

    /* vga.h's vga_canvas_t. Console (0) and 640x480 (3) decode alike. */
    localparam logic [2:0] CV_320_240 = 3'd1;
    localparam logic [2:0] CV_320_180 = 3'd2;
    localparam logic [2:0] CV_640_360 = 3'd4;

    function automatic logic cv_w320(input logic [2:0] cv);
        cv_w320 = cv == CV_320_240 || cv == CV_320_180;
    endfunction

    /* Which slots carry rows — the canvas's height, from the top. */
    function automatic logic [9:0] cv_ch(input logic [2:0] cv);
        cv_ch = cv == CV_320_240 ? 10'd240
            : cv == CV_320_180 ? 10'd180
            : cv == CV_640_360 ? 10'd360 : 10'(V_ACTIVE);
    endfunction
    function automatic logic cv_row_sel(input logic [2:0] cv,
                                        input logic [9:0] line);
        cv_row_sel = line < cv_ch(cv);
    endfunction

    /* The frame's first pixel, marked on the way in. The frame pulse
     * arms it and the next de spends it, and those never coincide: the
     * pulse is a pixel's first clock and de is its last. */
    logic sof_arm;
    initial sof_arm = 1'b0;
    always_ff @(posedge clk_mach) begin
        if (vid_frame)
            sof_arm <= 1'b1;
        else if (vid_de)
            sof_arm <= 1'b0;
    end

    /* Canvas pixels cross through the FIFO — jitter-deep, because the
     * two sides run at the same rate. */
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

    /* The reader's raster: started by the first frame it lines up with
     * and free running at the writer's exact rate from then on. The
     * lock beside it is whether the pixels it is sending are the
     * machine's; without it the same raster goes out black. */
    logic raster, locked /*verilator public_flat_rd*/;
    logic [9:0] x /*verilator public_flat_rd*/;
    logic [9:0] y /*verilator public_flat_rd*/;

    /* The machine's clock state, in the reader's domain. */
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
    /* Armed at the first frame boundary. Nothing resets the beam, so at
     * power-on it starts mid-line and the two sides of the FIFO come up
     * in whatever order they come up in — a scanline of the black screen
     * the machine boots to. After that a full or empty FIFO is a real
     * defect and worth stopping on. */
    /* One driver: the clear outranks the arm, or the two blocks race
     * and the clear can lose entirely. A machine being stopped empties
     * the FIFO legitimately, so the check stands down until the first
     * whole frame after the clock returns. */
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
    /* Only a locked reader is entitled to a pixel. What it discards
     * while lining up is bounded by the queue and stops at empty. */
    always_ff @(posedge clk_vid) begin
        if (checked && run_v2 && locked && take && fifo_empty)
            $error("pocket_video: pixel fifo underflow");
    end
`endif

    /* The reader's copy of the canvas, crossed and held for a whole
     * frame so the window cannot tear mid-picture. Taken at the
     * reader's frame boundaries, and the moment the lock is taken is
     * one of them: the writer changed canvas at the frame being locked
     * to — a restore does exactly that — and a reader still on the old
     * width would spend a whole frame popping a different number of
     * pixels than were pushed. */
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

    logic [9:0] cw;   /* the canvas's width: the de run and the pops */
    logic [2:0] slot; /* video.json's scaler_modes, same order */
    always_comb begin
        cw = cv_w320(canvas) ? 10'd320 : 10'd640;
        unique case (canvas)
            CV_640_360: slot = 3'd1;
            CV_320_240: slot = 3'd2;
            CV_320_180: slot = 3'd3;
            default: slot = 3'd0; /* console and 640x480 */
        endcase
    end

    /* de is the canvas, in the same slot the machine sends it: its own
     * width, back to back, one pop per asserted cycle. It belongs to
     * the raster and not to the lock — the scaler is owed the same
     * shape of frame either way — so while unlocked the same run of
     * cycles goes out black. The pops go to lining up instead: whatever
     * the frozen beam left behind, and whatever the resumed one pushes
     * before the frame it will be locked to, is discarded, and the
     * discarding stops on the tag. */
    logic de_sel;
    always_comb de_sel = raster
        && x >= 10'(X_DE0) && x < 10'(X_DE0) + cw
        && cv_row_sel(canvas, y);
    /* While the machine has no clock the tag is discarded along with
     * everything else. A frame that had only just begun when the beam
     * stopped leaves its first pixel, tag and all, at the head; kept,
     * it would be taken for the start of the frame the writer resumes
     * into, which is somewhere in the middle of one. */
    always_comb take = locked ? de_sel
        : (!fifo_empty && (!run_v2 || !fifo_sof));

    /* The end-of-line word: the cycle after de falls on every row, rgb
     * names the scaler slot — endline[23:13] the parameter, [2:0] the
     * Set Scaler Slot function code, zeros. Zeros anywhere else are
     * safe: the scaler samples this position only. */
    logic endline_now;
    always_comb endline_now = raster && cv_row_sel(canvas, y)
        && x == 10'(X_DE0) + cw;

    logic [7:0] r8, g8, b8;
    always_comb begin
        r8 = {fifo_pixel[4:0], fifo_pixel[4:2]};
        g8 = {fifo_pixel[10:6], fifo_pixel[10:8]};
        b8 = {fifo_pixel[15:11], fifo_pixel[15:13]};
    end

    /* The frame's first pixel is at the head and the machine has a
     * clock to have put it there. The raster restarts here and nowhere
     * else; the vs that would have fallen in between is simply late,
     * which is one long frame and not a lost one. */
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

        /* Taking the lock starts a frame, so it is announced as one.
         * The frame it interrupts is short by however far into it the
         * writer's boundary fell, which is one glitched frame at a
         * wake — against a de the scaler was never told to expect,
         * which is a scaler that has lost count. */
        pocket_video_vs <= relock || (raster && x == 10'(H_TOTAL - 1)
                                      && y == 10'(V_TOTAL - 1));
        /* One hs per slot: the order on the wire is always hs,
         * pixels, porch. */
        pocket_video_hs <= raster && x == 10'd2;
        pocket_video_de <= de_sel;
        pocket_video_rgb <= de_sel ? (locked ? {r8, g8, b8} : 24'h0)
            : endline_now ? {11'(slot), 13'd0}
            : 24'h0;
    end

    /* Nothing is skipped: only canvas pixels ever arrive. */
    always_comb pocket_video_skip = 1'b0;

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_video;
    always_comb unused_pocket_video = fifo_pixel[5]; /* the alpha bit */
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
