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
    /* clk_a2 clocks XRAM's render port at twice clk_sys, shifted so that
     * both of its edges fall clear of the clk_sys edges the machine's
     * registers change on. clk_ph is clk_sys shifted to be low across the
     * first of those edges and high across the second; xram.sv samples
     * it as data to tell them apart. */
    output wire clk_a2,
    output wire clk_ph,
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
    /* A phase shift is a whole number of eighths of the VCO's 827 ps
     * period, 103.3 ps. 8990 ps, 87 of them, puts clk_a2's edges 9.0 and
     * 18.9 ns after each clk_sys edge, where XRAM's registered addresses
     * and its outgoing words have equal margins. 12400 ps of clk_sys's
     * period puts clk_ph's rise 6.5 ns before the second of those edges
     * and its fall 6.5 ns before the first. */
    localparam A2_SHIFT = "8990 ps";
    localparam PH_SHIFT = "12400 ps";

    wire [6:0] outclk;
    assign clk_sys  = outclk[0];
    assign clk_vid  = outclk[1];
    assign clk_dram = outclk[2];
    assign clk_vid_90 = outclk[3];
    assign clk_rv   = outclk[4];
    assign clk_a2   = outclk[5];
    assign clk_ph   = outclk[6];

    altera_pll #(
        .fractional_vco_multiplier("true"),
        .reference_clock_frequency("74.25 MHz"),
        .operation_mode("normal"),
        .number_of_clocks(7),
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
        .output_clock_frequency5("100.800000 MHz"),
        .phase_shift5(A2_SHIFT),
        .duty_cycle5(50),
        .output_clock_frequency6("50.400000 MHz"),
        .phase_shift6(PH_SHIFT),
        .duty_cycle6(50),
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
