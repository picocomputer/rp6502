/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502 clock enable, as a phase accumulator rather than a divider.
 * Add the wanted kilohertz every system clock, and each time the total
 * reaches the system clock's own kilohertz, take that much back out and
 * pulse. The pulse rate is then exactly the wanted rate, for every whole
 * kilohertz the machine allows, with no quantization to apologize for
 * and nothing for the firmware to divide.
 *
 * The RIA arrives at the same place from the other side. Its PIO divider
 * is 16.8 fixed point, which would be inexact here — 50400/8000 is 6.3,
 * and 0.3 is not a binary fraction — but its clock is picked so that the
 * division comes out whole and the fixed point never has to express
 * anything awkward. The video picks ours, so the arithmetic gives way
 * instead of the frequency.
 *
 * Individual periods still vary by one system clock, exactly as the
 * RIA's do whenever its fraction is non-zero. It is the average that is
 * exact, and the 6502 is a clock-enable design that cannot tell.
 */

module phi2_div #(
    /* The clock being counted, in kHz. Every rate up to it is exact. */
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
