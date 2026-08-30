/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Silence, at the resampler's ports. See README.md beside this file.
 */

module aud_rsmp (
    input logic clk,
    input logic signed [15:0] in_sample,
    input logic in_valid,
    input logic step,
    output logic signed [15:0] aud_rsmp_out,
    output logic aud_rsmp_valid
);

    always_comb begin
        aud_rsmp_out = '0;
        aud_rsmp_valid = 1'b0;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_aud_rsmp;
    always_comb unused_aud_rsmp = clk ^ (^in_sample) ^ in_valid ^ step;
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
