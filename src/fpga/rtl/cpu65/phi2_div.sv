/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502 clock enable: one pulse every int + frac/256 system clocks, the
 * same 16.8 fixed-point shape as the RIA's PIO divider (ria/sys/cpu.c), so
 * every achievable PHI2 is exact and firmware quantization carries over.
 */

module phi2_div (
    input logic clk,
    input logic rst_n,

    input logic [15:0] div_int,
    input logic [7:0] div_frac,

    output logic phi2_div_en
);

    logic [15:0] count;
    logic [8:0] acc;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            count <= 16'd0;
            acc <= 9'd0;
            phi2_div_en <= 1'b0;
        end else if (count == 16'd0) begin
            phi2_div_en <= 1'b1;
            acc <= {1'b0, acc[7:0]} + {1'b0, div_frac};
            // The fractional carry stretches this period by one clock.
            count <= div_int - 16'd1 + {15'd0, acc[8]};
        end else begin
            phi2_div_en <= 1'b0;
            count <= count - 16'd1;
        end
    end

endmodule
