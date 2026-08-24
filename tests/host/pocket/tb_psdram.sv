/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Bench top: the controller against the behavioral chip, request
 * ports out to C++.
 */

module tb_psdram (
    input logic clk,
    input logic rst_n,

    input logic rd_pend,
    input logic [24:0] rd_addr,
    output logic [15:0] tb_psdram_rdata,
    output logic tb_psdram_rvalid,

    input logic w_avail,
    input logic [24:0] w_addr,
    input logic [15:0] w_data,
    output logic tb_psdram_wtake,

    output logic tb_psdram_ready,
    output logic [31:0] tb_psdram_refreshes,
    output logic [31:0] tb_psdram_sref_clocks
);

    logic cke;
    logic [12:0] a;
    logic [1:0] ba;
    logic [1:0] dqm;
    logic ras_n, cas_n, we_n;
    logic [15:0] dq_c2m, dq_m2c;
    logic dq_oe;

    pocket_sdram ctrl (
        .clk(clk),
        .rd_pend(rd_pend),
        .rd_addr(rd_addr),
        .pocket_sdram_rdata(tb_psdram_rdata),
        .pocket_sdram_rvalid(tb_psdram_rvalid),
        .w_avail(w_avail),
        .w_addr(w_addr),
        .w_data(w_data),
        .pocket_sdram_wtake(tb_psdram_wtake),
        .pocket_sdram_ready(tb_psdram_ready),
        .dram_cke(cke),
        .dram_a(a),
        .dram_ba(ba),
        .dram_dqm(dqm),
        .dram_ras_n(ras_n),
        .dram_cas_n(cas_n),
        .dram_we_n(we_n),
        .dram_dq_out(dq_c2m),
        .dram_dq_oe(dq_oe),
        .dram_dq_in(dq_m2c)
    );

    sdram_model chip (
        .clk(clk),
        .rst_n(rst_n),
        .cke(cke),
        .a(a),
        .ba(ba),
        .ras_n(ras_n),
        .cas_n(cas_n),
        .we_n(we_n),
        .dq_in(dq_c2m),
        .dq_oe(dq_oe),
        .dq_out(dq_m2c),
        .sdram_model_refreshes(tb_psdram_refreshes),
        .sdram_model_sref_clocks(tb_psdram_sref_clocks)
    );

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_tb_psdram;
    always_comb unused_tb_psdram = ^dqm;
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
