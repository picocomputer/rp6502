/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Silence, at the real PSG's ports. See README.md beside this file.
 *
 * The tick is kept, and kept at the right rate: it is the machine's
 * sample strobe, so the i2s path downstream still runs at 48 kHz and
 * behaves as it does in a real core. Only the sound is gone.
 */

module aud_psg (
    input logic clk,
    input logic xaddr_we,
    input logic [15:0] xaddr_wdata,
    input logic gate_any_we,
    input logic gate_any_wdata,
    input logic q_we,
    input logic q_host,
    input logic [15:0] q_addr,
    input logic [7:0] q_val,
    input logic bel_lo_we,
    input logic bel_hi_we,
    input logic [31:0] bel_wdata,
    output logic signed [15:0] aud_psg_l,
    output logic signed [15:0] aud_psg_r,
    output logic aud_psg_valid,
    output logic aud_psg_tick
);

    /* 50.4 MHz / 1050 = 48 kHz, the divider the real engine keeps. */
    localparam int TICK_DIV = 1050;
    logic [10:0] div;
    initial div = '0;
    initial aud_psg_tick = 1'b0;
    always_ff @(posedge clk) begin
        aud_psg_tick <= div == TICK_DIV - 1;
        div <= div == TICK_DIV - 1 ? '0 : div + 1'b1;
    end

    always_comb begin
        aud_psg_l = '0;
        aud_psg_r = '0;
        aud_psg_valid = 1'b0;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_aud_psg;
    always_comb unused_aud_psg = xaddr_we ^ (^xaddr_wdata) ^ gate_any_we
        ^ gate_any_wdata ^ q_we ^ q_host ^ (^q_addr) ^ (^q_val)
        ^ bel_lo_we ^ bel_hi_we ^ (^bel_wdata);
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
