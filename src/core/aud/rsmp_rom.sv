/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * rsmp_coef_pkg's rows 0..64 with two read ports, one for rsmp and one
 * for pcm. A row past 64 is a stored row read backwards. Reads are
 * registered: a port's q is the coefficient at the previous clock's row
 * and tap.
 */

module rsmp_rom
    import rsmp_coef_pkg::*;
(
    input logic clk,

    input logic [7:0] a_row,
    input logic [4:0] a_tap,
    output logic signed [RSMP_COEF_W-1:0] rsmp_rom_a_q,

    input logic [7:0] b_row,
    input logic [4:0] b_tap,
    output logic signed [RSMP_COEF_W-1:0] rsmp_rom_b_q
);

    localparam int AW = $clog2(RSMP_HALF_N);

    logic signed [RSMP_COEF_W-1:0] coef[RSMP_HALF_N];
    initial begin
        for (int i = 0; i < RSMP_HALF_N; i++) coef[i] = RSMP_HALF[i];
    end

    function automatic logic [AW-1:0] addr(input logic [7:0] row,
                                           input logic [4:0] tap);
        logic [7:0] r;
        logic [4:0] t;
        if (row <= 8'(RSMP_PHASES / 2)) begin
            r = row;
            t = tap;
        end else begin
            r = 8'(RSMP_PHASES) - row;
            t = 5'(RSMP_TAPS - 1) - tap;
        end
        return AW'(r) * AW'(RSMP_TAPS) + AW'(t);
    endfunction

    always_ff @(posedge clk) begin
        rsmp_rom_a_q <= coef[addr(a_row, a_tap)];
        rsmp_rom_b_q <= coef[addr(b_row, b_tap)];
    end

endmodule
