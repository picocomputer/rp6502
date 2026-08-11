/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module phi2_div #(
    parameter int SYS_KHZ = 50400
) (
    input logic clk,

    input logic [15:0] phi2_khz,

    output logic phi2_div_en
);

    localparam int ACC_W = $clog2(SYS_KHZ) + 1;

    logic [ACC_W-1:0] acc, next;
    always_comb next = acc + ACC_W'(phi2_khz);

    initial begin
        acc = '0;
        phi2_div_en = 1'b0;
    end
    always_ff @(posedge clk) begin
        if (next >= ACC_W'(SYS_KHZ)) begin
            acc <= next - ACC_W'(SYS_KHZ);
            phi2_div_en <= 1'b1;
        end else begin
            acc <= next;
            phi2_div_en <= 1'b0;
        end
    end

endmodule
