/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * rsmp and pcm on one rsmp_rom, as wiring.sv connects them.
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
    input logic [31:0] wdata,
    output logic [12:0] pcm_level,
    /* pcm's outputs, and its blend before the rounding shift that its
     * sum state takes, sign-extended. */
    output logic signed [31:0] pcm_l,
    output logic signed [31:0] pcm_r,
    output logic signed [63:0] pcm_v
);

    logic [7:0] rsmp_coef_row, pcm_coef_row;
    logic [4:0] rsmp_coef_tap, pcm_coef_tap;
    logic signed [RSMP_COEF_W-1:0] rsmp_rom_a_q, rsmp_rom_b_q;
    rsmp_rom rsmp_rom (
        .clk(clk),
        .a_row(rsmp_coef_row),
        .a_tap(rsmp_coef_tap),
        .rsmp_rom_a_q(rsmp_rom_a_q),
        .b_row(pcm_coef_row),
        .b_tap(pcm_coef_tap),
        .rsmp_rom_b_q(rsmp_rom_b_q)
    );

    rsmp rsmp (
        .clk(clk),
        .in_sample(in_sample),
        .in_valid(in_valid),
        .step(step),
        .rsmp_coef_row(rsmp_coef_row),
        .rsmp_coef_tap(rsmp_coef_tap),
        .coef_q(rsmp_rom_a_q),
        .rsmp_out(rsmp_out),
        .rsmp_valid(rsmp_valid)
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
        .wdata(wdata),
        .pcm_coef_row(pcm_coef_row),
        .pcm_coef_tap(pcm_coef_tap),
        .coef_q(rsmp_rom_b_q),
        .pcm_level(pcm_level),
        .pcm_l(pcm_out_l),
        .pcm_r(pcm_out_r)
    );
    always_comb begin
        pcm_l = 32'(pcm_out_l);
        pcm_r = 32'(pcm_out_r);
        pcm_v = 64'(pcm.v);
    end

endmodule
