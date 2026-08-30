/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Silence, at the OPL2's ports. See README.md beside this file.
 *
 * This is the one that matters for area: the YM3812 and its resampler
 * are the largest thing in the machine, and none of it bears on the
 * question a diagnostic bitstream is built to answer.
 */

module aud_opl (
    input logic clk,
    input logic xaddr_we,
    input logic [15:0] xaddr_wdata,
    input logic q_we,
    input logic [15:0] q_addr,
    input logic [7:0] q_val,
    output logic signed [15:0] aud_opl_out,
    output logic aud_opl_valid,
    output logic aud_opl_enabled
);

    always_comb begin
        aud_opl_out = '0;
        aud_opl_valid = 1'b0;
        aud_opl_enabled = 1'b0;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_aud_opl;
    always_comb unused_aud_opl = clk ^ xaddr_we ^ (^xaddr_wdata) ^ q_we
        ^ (^q_addr) ^ (^q_val);
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
