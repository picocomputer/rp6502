/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pixel clock is 25.2 MHz rather than 25.175 MHz because the RP2350
 * runs its 640x480 mode at 25.2 MHz, which is exactly 60 frames per
 * second over 800x525 pixels, and the FPGA reproduces the same timing.
 */

`timescale 1 ps / 1 ps

module pocket_pll (
    input  wire refclk,
    input  wire rst,
    output wire clk_sys,
    output wire clk_vid,
    output wire clk_dram,
    output wire clk_vid_90,
    /* clk_rv is half the rate of clk_sys and rises with it. It comes
     * from the PLL rather than from a flip-flop dividing clk_sys,
     * because a divided clock rises after the clk_sys registers have
     * changed at the same edge, and the soft CPU would capture their new
     * values instead of the ones they held before that edge. */
    output wire clk_rv,
    output wire locked
);

    /* 9920 ps is 180 degrees of the 19841 ps period of 50.4 MHz. The
     * shift puts each rising edge of the SDRAM's clock midway between
     * the clk_sys edges that launch pocket_sdram's outputs. */
    localparam DRAM_SHIFT = "9920 ps";
    /* 9921 ps is 90 degrees of the 39683 ps period of 25.2 MHz.
     * apf_top.v sends the pixels to the scaler as DDR launched on
     * clk_vid and drives the scaler's clock pin from clk_vid_90, so the
     * shift puts each edge of that clock in the middle of a half-period
     * data window. */
    localparam VID_SHIFT = "9921 ps";

    wire [4:0] outclk;
    assign clk_sys  = outclk[0];
    assign clk_vid  = outclk[1];
    assign clk_dram = outclk[2];
    assign clk_vid_90 = outclk[3];
    assign clk_rv   = outclk[4];

    altera_pll #(
        .fractional_vco_multiplier("true"),
        .reference_clock_frequency("74.25 MHz"),
        .operation_mode("normal"),
        .number_of_clocks(5),
        .output_clock_frequency0("50.400000 MHz"),
        .phase_shift0("0 ps"),
        .duty_cycle0(50),
        .output_clock_frequency1("25.200000 MHz"),
        .phase_shift1("0 ps"),
        .duty_cycle1(50),
        .output_clock_frequency2("50.400000 MHz"),
        .phase_shift2(DRAM_SHIFT),
        .duty_cycle2(50),
        .output_clock_frequency3("25.200000 MHz"),
        .phase_shift3(VID_SHIFT),
        .duty_cycle3(50),
        .output_clock_frequency4("25.200000 MHz"),
        .phase_shift4("0 ps"),
        .duty_cycle4(50),
        .pll_type("General"),
        .pll_subtype("General")
    ) pll (
        .rst(rst),
        .outclk(outclk),
        .locked(locked),
        .fboutclk(),
        .fbclk(1'b0),
        .refclk(refclk)
    );

endmodule
