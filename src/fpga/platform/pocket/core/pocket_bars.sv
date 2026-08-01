/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Colour bars straight at the scaler, for the first bring-up of a board
 * that shows nothing. It stands in for the whole machine: if the bars
 * appear, then the PLL, the pin assignments, the bitstream's bit order
 * and the scaler handshake are all good and the fault is inside the
 * machine. If they do not, none of those are proven and the machine is
 * beside the point.
 *
 * The timing is pocket_video's, deliberately — vs a single clock at the
 * last pixel of the last line, hs a single clock at x == 2, and the
 * active window starting eight pixels in. The scaler wants pulses, not
 * the sync levels a VGA monitor would; a generator written the VGA way
 * is black for a reason that has nothing to do with what is being
 * tested.
 */

module pocket_bars (
    input logic clk_vid,
    input logic rst_n,

    output logic [23:0] pocket_bars_rgb,
    output logic pocket_bars_de,
    output logic pocket_bars_skip,
    output logic pocket_bars_vs,
    output logic pocket_bars_hs
);

    localparam int H_TOTAL = 800;
    localparam int V_TOTAL = 525;
    localparam int X_DE0 = 8;
    localparam int H_ACTIVE = 640;
    localparam int V_ACTIVE = 480;

    logic [9:0] x, y;

    always_ff @(posedge clk_vid or negedge rst_n) begin
        if (!rst_n) begin
            x <= '0;
            y <= '0;
            pocket_bars_vs <= 1'b0;
            pocket_bars_hs <= 1'b0;
        end else begin
            if (x == 10'(H_TOTAL - 1)) begin
                x <= '0;
                y <= y == 10'(V_TOTAL - 1) ? '0 : y + 10'd1;
            end else
                x <= x + 10'd1;
            pocket_bars_vs <= x == 10'(H_TOTAL - 1) && y == 10'(V_TOTAL - 1);
            pocket_bars_hs <= x == 10'd2;
        end
    end

    logic [9:0] px;
    always_comb px = x - 10'(X_DE0);

    /* Eight bars across, and a moving line down the frame so a static
     * picture can be told from a stuck one. */
    logic [2:0] bar;
    always_comb bar = 3'(px / 10'd80);

    always_comb begin
        pocket_bars_de = x >= 10'(X_DE0) && x < 10'(X_DE0 + H_ACTIVE)
            && y < 10'(V_ACTIVE);
        pocket_bars_skip = 1'b0;
        pocket_bars_rgb = {{8{bar[2]}}, {8{bar[1]}}, {8{bar[0]}}};
    end

endmodule
