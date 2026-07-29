/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The core's clocks, all from the one VCO so they are edge aligned and
 * the crossings between them are ratios rather than domains.
 *
 *   74.25 MHz in, multiplied by 448 over 33, is 1008 MHz exactly
 *     / 10  ->  100.8 MHz   the machine
 *     / 40  ->   25.2 MHz   the beam, and the scaler's sample clock
 *     / 10  ->  100.8 MHz   phase shifted, out to the memory chip
 *
 * 25.2 MHz rather than 25.175 is the machine's own choice, not a
 * rounding: the RP2350 runs the CEA variant of 640x480 at exactly sixty
 * frames, and the FPGA is the same machine.
 *
 * The memory's clock is shifted because the chip samples what the
 * controller launched, and on the same edge there is no setup at all.
 * Half a period puts the sample in the middle of the window, which is
 * the conventional starting point and the first thing to sweep on real
 * hardware — the trace lengths on the board are not in this file.
 */

`timescale 1 ps / 1 ps

module pocket_pll (
    input  wire refclk,
    input  wire rst,
    output wire clk_sys,   // 100.8 MHz
    output wire clk_vid,   //  25.2 MHz
    output wire clk_dram,  // 100.8 MHz, half a period late
    output wire locked
);

    /* 180 degrees of a 100.8 MHz period. */
    localparam DRAM_SHIFT = "4960 ps";

    wire [4:0] outclk;
    assign clk_sys  = outclk[0];
    assign clk_vid  = outclk[1];
    assign clk_dram = outclk[2];

    altera_pll #(
        .fractional_vco_multiplier("true"),
        .reference_clock_frequency("74.25 MHz"),
        .operation_mode("normal"),
        .number_of_clocks(3),
        .output_clock_frequency0("100.800000 MHz"),
        .phase_shift0("0 ps"),
        .duty_cycle0(50),
        .output_clock_frequency1("25.200000 MHz"),
        .phase_shift1("0 ps"),
        .duty_cycle1(50),
        .output_clock_frequency2("100.800000 MHz"),
        .phase_shift2(DRAM_SHIFT),
        .duty_cycle2(50),
        .output_clock_frequency3("0 MHz"),
        .phase_shift3("0 ps"),
        .duty_cycle3(50),
        .output_clock_frequency4("0 MHz"),
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
