/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * pcm, the OPL2's resampler and PCM on one filter, as wiring.sv connects
 * it. wdata carries the savestate's word too, as in wiring.sv.
 */

module tb_aud
    import rsmp_coef_pkg::*;
(
    input logic clk,

    input logic signed [15:0] in_sample,
    input logic in_valid,
    input logic step,
    output logic signed [15:0] rsmp_out,
    output logic rsmp_valid,

    input logic tick,
    input logic ctrl_we,
    input logic step_we,
    input logic data_we,
    input logic byte_we,
    input logic clear_we,
    input logic commit_we,
    input logic [31:0] wdata,
    input logic under_take,
    output logic [12:0] pcm_level,
    output logic [30:0] pcm_under,
    /* pcm's outputs, and its blend before the rounding shift that its
     * sum state takes, sign-extended. */
    output logic signed [31:0] pcm_l,
    output logic signed [31:0] pcm_r,
    output logic signed [63:0] pcm_v,

    input logic sst_own,
    input logic [10:0] sst_addr,
    input logic sst_we,
    output logic [31:0] rsmp_sst_rdata,
    output logic [31:0] pcm_sst_rdata,
    output logic rsmp_quiet,
    output logic pcm_quiet
);

    logic signed [RSMP_OUT_W-1:0] pcm_out_l, pcm_out_r;
    pcm pcm (
        .clk(clk),
        .tick(tick),
        .ctrl_we(ctrl_we),
        .step_we(step_we),
        .data_we(data_we),
        .byte_we(byte_we),
        .clear_we(clear_we),
        .commit_we(commit_we),
        .wdata(wdata),
        .under_take(under_take),
        .pcm_level(pcm_level),
        .pcm_under(pcm_under),
        .pcm_l(pcm_out_l),
        .pcm_r(pcm_out_r),
        .in_sample(in_sample),
        .in_valid(in_valid),
        .step(step),
        .rsmp_out(rsmp_out),
        .rsmp_valid(rsmp_valid),
        .sst_own(sst_own),
        .sst_addr(sst_addr),
        .sst_we(sst_we),
        .pcm_sst_rdata(pcm_sst_rdata),
        .pcm_quiet(pcm_quiet)
    );
    /* The OPL2's stream is pcm's too, so its savestate words come out on
     * pcm_sst_rdata. */
    always_comb begin
        rsmp_sst_rdata = '0;
        rsmp_quiet = 1'b1;
    end
    always_comb begin
        pcm_l = 32'(pcm_out_l);
        pcm_r = 32'(pcm_out_r);
        pcm_v = 64'(pcm.v);
    end

endmodule
