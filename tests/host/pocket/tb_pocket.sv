/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Bench top: pocket_core with the behavioral SDRAM behind its pads.
 */

module tb_pocket (
    input logic clk_74a,
    input logic clk_sys,
    /* Half clk_sys and rising with it, as the PLL makes it. */
    input logic clk_rv,
    input logic clk_vid,
    input logic rst_n,
    input logic arst_n,

    input logic bridge_wr,
    input logic bridge_rd,
    input logic [31:0] bridge_addr,
    input logic [31:0] bridge_wr_data,
    input logic dataslot_allcomplete,
    input logic reset_n,
    output logic [9:0] tb_pocket_dt_addr,
    input logic [31:0] datatable_q,
    /* The host's clock, as command 0x0090 would have latched it. */
    input logic [31:0] rtc_epoch,
    input logic rtc_valid,

    /* The file bridge, played from the bench the way the host plays it. */
    output logic [31:0] tb_pocket_bridge_rd_data,
    output logic [31:0] tb_pocket_param_struct,
    output logic [31:0] tb_pocket_resp_struct,
    output logic tb_pocket_ds_read,
    output logic tb_pocket_ds_write,
    output logic tb_pocket_ds_openfile,
    output logic tb_pocket_ds_getfile,
    output logic tb_pocket_ds_flush,
    output logic [15:0] tb_pocket_ds_id,
    output logic [31:0] tb_pocket_ds_slotoffset,
    output logic [31:0] tb_pocket_ds_bridgeaddr,
    output logic [31:0] tb_pocket_ds_length,
    input logic target_dataslot_done,
    input logic [2:0] target_dataslot_err,

    input logic [31:0] cont1_key,
    input logic [31:0] cont1_joy,
    input logic [15:0] cont1_trig,
    input logic [31:0] cont3_key,
    input logic [31:0] cont3_joy,
    input logic [15:0] cont3_trig,
    input logic [31:0] cont4_key,
    input logic [31:0] cont4_joy,
    input logic [15:0] cont4_trig,

    output logic [23:0] tb_pocket_rgb,
    output logic tb_pocket_de,
    output logic tb_pocket_skip,
    output logic tb_pocket_vs,
    output logic tb_pocket_hs,

    output logic tb_pocket_mclk,
    output logic tb_pocket_dac,
    output logic tb_pocket_lrck,

    output logic tb_pocket_ready,
    output logic [7:0] tb_pocket_tx_data,
    output logic tb_pocket_tx_valid,
    output logic [7:0] tb_pocket_rv_tx_data,
    output logic tb_pocket_rv_tx_valid,
    output logic tb_pocket_rv_halted
);

    logic cke;
    logic [12:0] a;
    logic [1:0] ba;
    logic [1:0] dqm;
    logic ras_n, cas_n, we_n;
    logic [15:0] dq_c2m, dq_m2c;
    logic dq_oe;
    logic [31:0] refreshes;
    logic [31:0] sref_clocks;

    pocket_core core (
        .clk_74a(clk_74a),
        .clk_sys(clk_sys),
        .clk_rv(clk_rv),
        .clk_vid(clk_vid),
        .rst_n(rst_n),
        .arst_n(arst_n),
        .bridge_wr(bridge_wr),
        .bridge_addr(bridge_addr),
        .bridge_rd(bridge_rd),
        .bridge_wr_data(bridge_wr_data),
        .dataslot_allcomplete(dataslot_allcomplete),
        .reset_n(reset_n),
        .pocket_core_dt_addr(tb_pocket_dt_addr),
        .datatable_q(datatable_q),
        .rtc_epoch(rtc_epoch),
        .rtc_valid(rtc_valid),
        .pocket_core_bridge_rd_data(tb_pocket_bridge_rd_data),
        .pocket_core_param_struct(tb_pocket_param_struct),
        .pocket_core_resp_struct(tb_pocket_resp_struct),
        .pocket_core_dataslot_read(tb_pocket_ds_read),
        .pocket_core_dataslot_write(tb_pocket_ds_write),
        .pocket_core_dataslot_openfile(tb_pocket_ds_openfile),
        .pocket_core_dataslot_getfile(tb_pocket_ds_getfile),
        .pocket_core_dataslot_flush(tb_pocket_ds_flush),
        .pocket_core_dataslot_id(tb_pocket_ds_id),
        .pocket_core_dataslot_slotoffset(tb_pocket_ds_slotoffset),
        .pocket_core_dataslot_bridgeaddr(tb_pocket_ds_bridgeaddr),
        .pocket_core_dataslot_length(tb_pocket_ds_length),
        .target_dataslot_done(target_dataslot_done),
        .target_dataslot_err(target_dataslot_err),
        .cont1_key(cont1_key),
        .cont1_joy(cont1_joy),
        .cont1_trig(cont1_trig),
        .cont3_key(cont3_key),
        .cont3_joy(cont3_joy),
        .cont3_trig(cont3_trig),
        .cont4_key(cont4_key),
        .cont4_joy(cont4_joy),
        .cont4_trig(cont4_trig),
        .pocket_core_rgb(tb_pocket_rgb),
        .pocket_core_de(tb_pocket_de),
        .pocket_core_skip(tb_pocket_skip),
        .pocket_core_vs(tb_pocket_vs),
        .pocket_core_hs(tb_pocket_hs),
        .pocket_core_mclk(tb_pocket_mclk),
        .pocket_core_dac(tb_pocket_dac),
        .pocket_core_lrck(tb_pocket_lrck),
        .dram_cke(cke),
        .dram_a(a),
        .dram_ba(ba),
        .dram_dqm(dqm),
        .dram_ras_n(ras_n),
        .dram_cas_n(cas_n),
        .dram_we_n(we_n),
        .dram_dq_out(dq_c2m),
        .dram_dq_oe(dq_oe),
        .dram_dq_in(dq_m2c),
        .sram_a(sram_a),
        .sram_dq_out(sram_dq_out),
        .sram_dq_oe(sram_dq_oe),
        .sram_dq_in(sram_dq),
        .sram_oe_n(sram_oe_n),
        .sram_we_n(sram_we_n),
        .sram_ub_n(sram_ub_n),
        .sram_lb_n(sram_lb_n),
        .pocket_core_ready(tb_pocket_ready),
        .pocket_core_tx_data(tb_pocket_tx_data),
        .pocket_core_tx_valid(tb_pocket_tx_valid),
        .pocket_core_rv_tx_data(tb_pocket_rv_tx_data),
        .pocket_core_rv_tx_valid(tb_pocket_rv_tx_valid),
        .pocket_core_rv_halted(tb_pocket_rv_halted)
    );

    /* The board's SRAM, holding the 6502's 64 KB. */
    wire [15:0] sram_dq;
    logic [16:0] sram_a;
    logic [15:0] sram_dq_out;
    logic sram_dq_oe, sram_oe_n, sram_we_n, sram_ub_n, sram_lb_n;
    assign sram_dq = sram_dq_oe ? sram_dq_out : {16{1'bz}};
    asram_model sram_chip (
        .a(sram_a),
        .dq(sram_dq),
        .oe_n(sram_oe_n),
        .we_n(sram_we_n),
        .ub_n(sram_ub_n),
        .lb_n(sram_lb_n)
    );

    sdram_model chip (
        .clk(clk_sys),
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
        .sdram_model_refreshes(refreshes),
        .sdram_model_sref_clocks(sref_clocks)
    );

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_tb_pocket;
    always_comb unused_tb_pocket = ^{dqm, refreshes, sref_clocks};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
