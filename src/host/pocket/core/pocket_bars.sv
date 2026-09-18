/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module pocket_bars (
    input logic clk_vid,

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

    initial begin
        x = '0;
        y = '0;
        pocket_bars_vs = 1'b0;
        pocket_bars_hs = 1'b0;
    end
    always_ff @(posedge clk_vid) begin
        if (x == 10'(H_TOTAL - 1)) begin
            x <= '0;
            y <= y == 10'(V_TOTAL - 1) ? '0 : y + 10'd1;
        end else
            x <= x + 10'd1;
        pocket_bars_vs <= x == 10'(H_TOTAL - 1) && y == 10'(V_TOTAL - 1);
        pocket_bars_hs <= x == 10'd2;
    end

    logic [9:0] px;
    always_comb px = x - 10'(X_DE0);

    logic [2:0] bar;
    always_comb bar = 3'(px / 10'd80);

    always_comb begin
        pocket_bars_de = x >= 10'(X_DE0) && x < 10'(X_DE0 + H_ACTIVE)
            && y < 10'(V_ACTIVE);
        pocket_bars_skip = 1'b0;
        pocket_bars_rgb = {{8{bar[2]}}, {8{bar[1]}}, {8{bar[0]}}};
    end

endmodule
