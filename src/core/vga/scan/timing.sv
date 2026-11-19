/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's beam: 800 pixels a line and 525 lines a frame, from a
 * clock at twice the pixel clock, 50.4 MHz against 25.2 MHz, which is
 * exactly 60 frames a second. Every pixel spans two clocks, so the
 * engines drawing a line have 1,600 clocks to do it in.
 *
 * The beam is the machine's, as the C renderer's is: on line N the
 * engines draw line N, the frame starts on line 0, and the VSYNC comes on
 * the last line drawn. What reaches the screen trails it by a line, and
 * host/pocket/core/wiring.sv keeps that line's count.
 *
 * timing_line_start and timing_frame_start fire on a pixel's first clock.
 */

module timing
    import timing_pkg::*;
(
    input logic clk,

    output logic [9:0] timing_h,
    output logic [9:0] timing_v,
    output logic timing_px_first,
    output logic timing_px_last,
    output logic timing_line_start,
    output logic timing_frame_start
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
        timing_line_start = timing_h == 10'd0 && tick == 1'd0;
        timing_frame_start = timing_h == 10'd0
            && timing_v == 10'd0 && tick == 1'd0;
    end

endmodule
