/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 640x480@60 raster, the only one this machine scans out: 800 clocks a
 * line, 525 lines a frame, syncs active-low per the VGA-side scanvideo
 * programming. One clock is one pixel; in simulation that clock is clk_sys,
 * on hardware this module moves to the 25.2 MHz pixel domain whole.
 *
 * vsync_pulse fires once per frame at the 479-to-480 transition — the
 * moment the emulator's run_frame counts a frame for $FFE3, a pacing
 * signal at the start of vblank, not a beam-position latch.
 */

module vid_timing
    import rp6502_pkg::*;
(
    input logic clk,
    input logic rst_n,

    output logic [9:0] vid_timing_h,
    output logic [9:0] vid_timing_v,
    output logic vid_timing_de,
    output logic vid_timing_hsync,
    output logic vid_timing_vsync,
    output logic vid_timing_line_start,
    output logic vid_timing_frame_start,
    output logic vid_timing_vsync_pulse
);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            vid_timing_h <= '0;
            vid_timing_v <= '0;
        end else if (vid_timing_h == 10'(RP6502_H_TOTAL - 1)) begin
            vid_timing_h <= '0;
            if (vid_timing_v == 10'(RP6502_V_TOTAL - 1))
                vid_timing_v <= '0;
            else
                vid_timing_v <= vid_timing_v + 10'd1;
        end else begin
            vid_timing_h <= vid_timing_h + 10'd1;
        end
    end

    always_comb begin
        vid_timing_de = vid_timing_h < 10'(RP6502_H_ACTIVE)
            && vid_timing_v < 10'(RP6502_V_ACTIVE);
        vid_timing_hsync = !(vid_timing_h >= 10'(RP6502_H_ACTIVE + RP6502_H_FP)
            && vid_timing_h < 10'(RP6502_H_ACTIVE + RP6502_H_FP + RP6502_H_SYNC));
        vid_timing_vsync = !(vid_timing_v >= 10'(RP6502_V_ACTIVE + RP6502_V_FP)
            && vid_timing_v < 10'(RP6502_V_ACTIVE + RP6502_V_FP + RP6502_V_SYNC));
        vid_timing_line_start = vid_timing_h == 10'd0;
        vid_timing_frame_start = vid_timing_h == 10'd0 && vid_timing_v == 10'd0;
        vid_timing_vsync_pulse = vid_timing_h == 10'd0
            && vid_timing_v == 10'(RP6502_V_ACTIVE);
    end

endmodule
