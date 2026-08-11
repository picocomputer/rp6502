/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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
