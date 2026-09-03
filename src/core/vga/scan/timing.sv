/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 640x480@60 raster, the only one this machine scans out: 800 pixels
 * a line, 525 lines a frame, syncs active-low per the VGA-side scanvideo
 * programming. The clock runs twice the pixel — the 50.4 MHz render
 * domain over the 25.2 MHz beam — so every pixel spans two clocks and
 * the engines racing the beam get their 1,600-clock line.
 * The pulses fire on a pixel's first clock; de strobes on its last, when
 * everything upstream has settled, so a de sample is one pixel.
 *
 * vsync_pulse fires once per frame at the 479-to-480 transition — the
 * moment the emulator's run_frame counts a frame for $FFE3, a pacing
 * signal at the start of vblank, not a beam-position latch.
 */

module timing
    import timing_pkg::*;
(
    input logic clk,

    output logic [9:0] timing_h,
    output logic [9:0] timing_v,
    output logic timing_px_first,
    output logic timing_px_last,
    output logic timing_de,
    output logic timing_hsync,
    output logic timing_vsync,
    output logic timing_line_start,
    output logic timing_frame_start,
    output logic timing_vsync_pulse
);

    logic tick;

    initial begin
        tick = '0;
        timing_h = '0;
        timing_v = '0;
    end
    always_ff @(posedge clk) begin
        tick <= tick + 1'd1;
        if (tick == 1'd1) begin
            if (timing_h == 10'(H_TOTAL - 1)) begin
                timing_h <= '0;
                if (timing_v == 10'(V_TOTAL - 1))
                    timing_v <= '0;
                else
                    timing_v <= timing_v + 10'd1;
            end else begin
                timing_h <= timing_h + 10'd1;
            end
        end
    end

    always_comb begin
        timing_px_first = tick == 1'd0;
        timing_px_last = tick == 1'd1;
        timing_de = timing_h < 10'(H_ACTIVE)
            && timing_v < 10'(V_ACTIVE) && tick == 1'd1;
        timing_hsync = !(timing_h >= 10'(H_ACTIVE + H_FP)
            && timing_h < 10'(H_ACTIVE + H_FP + H_SYNC));
        timing_vsync = !(timing_v >= 10'(V_ACTIVE + V_FP)
            && timing_v < 10'(V_ACTIVE + V_FP + V_SYNC));
        timing_line_start = timing_h == 10'd0 && tick == 1'd0;
        timing_frame_start = timing_h == 10'd0
            && timing_v == 10'd0 && tick == 1'd0;
        timing_vsync_pulse = timing_h == 10'd0
            && timing_v == 10'(V_ACTIVE) && tick == 1'd0;
    end

endmodule
