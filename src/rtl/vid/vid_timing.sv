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

module vid_timing
    import rp6502_pkg::*;
(
    input logic clk,

    output logic [9:0] vid_timing_h,
    output logic [9:0] vid_timing_v,
    output logic vid_timing_px_first,
    output logic vid_timing_px_last,
    output logic vid_timing_de,
    output logic vid_timing_hsync,
    output logic vid_timing_vsync,
    output logic vid_timing_line_start,
    output logic vid_timing_frame_start,
    output logic vid_timing_vsync_pulse
);

    logic tick;

    initial begin
        tick = '0;
        vid_timing_h = '0;
        vid_timing_v = '0;
    end
    always_ff @(posedge clk) begin
        tick <= tick + 1'd1;
        if (tick == 1'd1) begin
            if (vid_timing_h == 10'(RP6502_H_TOTAL - 1)) begin
                vid_timing_h <= '0;
                if (vid_timing_v == 10'(RP6502_V_TOTAL - 1))
                    vid_timing_v <= '0;
                else
                    vid_timing_v <= vid_timing_v + 10'd1;
            end else begin
                vid_timing_h <= vid_timing_h + 10'd1;
            end
        end
    end

    always_comb begin
        vid_timing_px_first = tick == 1'd0;
        vid_timing_px_last = tick == 1'd1;
        vid_timing_de = vid_timing_h < 10'(RP6502_H_ACTIVE)
            && vid_timing_v < 10'(RP6502_V_ACTIVE) && tick == 1'd1;
        vid_timing_hsync = !(vid_timing_h >= 10'(RP6502_H_ACTIVE + RP6502_H_FP)
            && vid_timing_h < 10'(RP6502_H_ACTIVE + RP6502_H_FP + RP6502_H_SYNC));
        vid_timing_vsync = !(vid_timing_v >= 10'(RP6502_V_ACTIVE + RP6502_V_FP)
            && vid_timing_v < 10'(RP6502_V_ACTIVE + RP6502_V_FP + RP6502_V_SYNC));
        vid_timing_line_start = vid_timing_h == 10'd0 && tick == 1'd0;
        vid_timing_frame_start = vid_timing_h == 10'd0
            && vid_timing_v == 10'd0 && tick == 1'd0;
        vid_timing_vsync_pulse = vid_timing_h == 10'd0
            && vid_timing_v == 10'(RP6502_V_ACTIVE) && tick == 1'd0;
    end

endmodule
