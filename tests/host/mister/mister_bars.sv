/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A test card in the shape of the machine's pixel stream: core/sys/
 * wiring.sv's vid_de, vid_out_v and vid_frame, the canvas taken as the
 * machine's frame starts, and compose's register on the pixel and de.
 * The top half of a canvas is eight bars, the bottom half carries the
 * row and the column, with bit 5 (alpha) toggling.
 */

module mister_bars
    import timing_pkg::*, canvas_pkg::*;
(
    input logic clk_sys,
    input logic [2:0] canvas,

    output logic [15:0] mister_bars_pixel,
    output logic mister_bars_de,
    output logic mister_bars_frame,
    output logic [2:0] mister_bars_canvas
);

    logic [9:0] h, v;
    logic px_first, px_last, line_start, frame_start;
    timing timing (
        .clk(clk_sys),
        .timing_h(h),
        .timing_v(v),
        .timing_px_first(px_first),
        .timing_px_last(px_last),
        .timing_line_start(line_start),
        .timing_frame_start(frame_start)
    );

    logic [2:0] cv;
    initial cv = '0;
    always_ff @(posedge clk_sys)
        if (frame_start)
            cv <= canvas;
    always_comb mister_bars_canvas = cv;

    logic [9:0] out_v, cw, ch, row;
    logic dbl, de;
    logic [2:0] bar;
    logic [15:0] pix;
    always_comb begin
        out_v = v == 10'd0 ? 10'(V_TOTAL - 1) : v - 10'd1;
        mister_bars_frame = line_start && v == 10'd1;
        cw = canvas_w(cv);
        ch = canvas_h(cv);
        dbl = cw == 10'd320;
        de = px_last && h < cw
            && (dbl ? out_v[0] && out_v < {ch[8:0], 1'b0} : out_v < ch);
        row = dbl ? out_v >> 1 : out_v;
        bar = 3'((dbl ? h << 1 : h) / 10'd80);
        pix = row < ch >> 1
            ? {{5{bar[2]}}, {5{bar[1]}}, 1'b0, {5{bar[0]}}}
            : {row[4:0], h[8:4], h[0], h[4:0]};
    end

    initial begin
        mister_bars_pixel = '0;
        mister_bars_de = 1'b0;
    end
    always_ff @(posedge clk_sys) begin
        mister_bars_pixel <= pix;
        mister_bars_de <= de;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_mister_bars;
    always_comb unused_mister_bars = ^{px_first, h[9], row[9:5]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
