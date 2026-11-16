/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * pocket_sst behind the core's own core_bridge_cmd, so the host's
 * savestate commands are sent as the Pocket sends them, as writes to
 * 0xF8000000, and their result codes are the ones the Pocket reads. Its
 * reads of the window come from the SDRAM controller and the chip model,
 * whose memory a test fills directly.
 *
 * core_bridge_cmd is Analogue's template's, as vendor/openfpga_rp6502
 * changes it, https://github.com/open-fpga/core-template. mf_datatable
 * and synch_3 stand in for the template's megafunction and common.v.
 */

/* core_bridge_cmd's data table, an Altera megafunction on hardware. */
module mf_datatable (
    input logic [9:0] address_a,
    input logic [9:0] address_b,
    input logic clock_a,
    input logic clock_b,
    input logic [31:0] data_a,
    input logic [31:0] data_b,
    input logic wren_a,
    input logic wren_b,
    output logic [31:0] q_a,
    output logic [31:0] q_b
);
    logic [31:0] mem[1024];
    always_ff @(posedge clock_a) begin
        if (wren_a) mem[address_a] <= data_a;
        q_a <= mem[address_a];
    end
    always_ff @(posedge clock_b) begin
        if (wren_b) mem[address_b] <= data_b;
        q_b <= mem[address_b];
    end
endmodule

/* The synchroniser of the template's common.v. */
module synch_3 (
    input logic i,
    output logic o,
    input logic clk
);
    logic s1, s2;
    always_ff @(posedge clk) {o, s2, s1} <= {s2, s1, i};
endmodule

module tb_psst (
    input logic clk_sys,
    input logic stb,
    input logic we,
    input logic [27:0] addr,
    input logic [31:0] wdata,
    output logic [31:0] tb_psst_rdata,
    input logic w_queued,

    input logic clk_74a,
    input logic arst_n,
    input logic bridge_wr,
    input logic bridge_rd,
    input logic [31:0] bridge_addr,
    input logic [31:0] bridge_wr_data,
    output logic [31:0] tb_psst_rd_data,
    output logic [31:0] tb_psst_cmd_rd_data,
    output logic tb_psst_reset_n,

    output logic tb_psst_start_busy,
    output logic tb_psst_start_ok,
    output logic tb_psst_start_err,
    output logic tb_psst_load_busy,
    output logic tb_psst_load_ok,
    output logic tb_psst_load_err,
    /* How many times each request line rose. */
    output logic [7:0] tb_psst_starts,
    output logic [7:0] tb_psst_loads,

    output logic tb_psst_sdram_ready
);

    logic savestate_start, savestate_start_ack;
    logic savestate_load, savestate_load_ack;
    logic rd_sel;

    logic start_q, load_q;
    initial begin
        start_q = 1'b0;
        load_q = 1'b0;
        tb_psst_starts = '0;
        tb_psst_loads = '0;
    end
    always_ff @(posedge clk_74a) begin
        start_q <= savestate_start;
        load_q <= savestate_load;
        if (savestate_start && !start_q) tb_psst_starts <= tb_psst_starts + 8'd1;
        if (savestate_load && !load_q) tb_psst_loads <= tb_psst_loads + 8'd1;
    end

    /* verilator lint_off PINCONNECTEMPTY */
    core_bridge_cmd cmd (
        .clk(clk_74a),
        .reset_n(tb_psst_reset_n),
        .bridge_endian_little(1'b0),
        .bridge_addr(bridge_addr),
        .bridge_rd(bridge_rd),
        .bridge_rd_data(tb_psst_cmd_rd_data),
        .bridge_wr(bridge_wr),
        .bridge_wr_data(bridge_wr_data),
        .status_boot_done(1'b1),
        .status_setup_done(1'b1),
        .status_running(tb_psst_reset_n),
        .dataslot_requestread(),
        .dataslot_requestread_id(),
        .dataslot_requestread_ack(1'b1),
        .dataslot_requestread_ok(1'b1),
        .dataslot_requestwrite(),
        .dataslot_requestwrite_id(),
        .dataslot_requestwrite_size(),
        .dataslot_requestwrite_ack(1'b1),
        .dataslot_requestwrite_ok(1'b1),
        .dataslot_update(),
        .dataslot_update_id(),
        .dataslot_update_size(),
        .dataslot_allcomplete(),
        .rtc_epoch_seconds(),
        .rtc_date_bcd(),
        .rtc_time_bcd(),
        .rtc_valid(),
        .savestate_supported(1'b1),
        .savestate_addr(32'h03EC_1000),
        .savestate_size(pocket_version_pkg::SST_BYTES),
        .savestate_maxloadsize(32'd1306624),
        .osnotify_inmenu(),
        .savestate_start(savestate_start),
        .savestate_start_ack(savestate_start_ack),
        .savestate_start_busy(tb_psst_start_busy),
        .savestate_start_ok(tb_psst_start_ok),
        .savestate_start_err(tb_psst_start_err),
        .savestate_load(savestate_load),
        .savestate_load_ack(savestate_load_ack),
        .savestate_load_busy(tb_psst_load_busy),
        .savestate_load_ok(tb_psst_load_ok),
        .savestate_load_err(tb_psst_load_err),
        .target_dataslot_read(1'b0),
        .target_dataslot_write(1'b0),
        .target_dataslot_getfile(1'b0),
        .target_dataslot_openfile(1'b0),
        .target_dataslot_ack(),
        .target_dataslot_done(),
        .target_dataslot_err(),
        .target_dataslot_id(16'd0),
        .target_dataslot_slotoffset(32'd0),
        .target_dataslot_bridgeaddr(32'd0),
        .target_dataslot_length(32'd0),
        .target_debug_event(1'b0),
        .target_debug_id(32'd0),
        .target_debug_done(),
        .target_buffer_param_struct(32'd0),
        .target_buffer_resp_struct(32'd0),
        .datatable_addr(10'd0),
        .datatable_wren(1'b0),
        .datatable_data(32'd0),
        .datatable_q()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    logic rd_pend, rvalid;
    logic [23:0] rd_addr;
    logic [31:0] rword;

    pocket_sst sst (
        .clk_sys(clk_sys),
        .stb(stb),
        .we(we),
        .addr(addr),
        .wdata(wdata),
        .pocket_sst_rdata(tb_psst_rdata),
        .w_queued(w_queued),
        .pocket_sst_rd_pend(rd_pend),
        .pocket_sst_rd_addr(rd_addr),
        .rd_rvalid(rvalid),
        .rd_rword(rword),
        .clk_74a(clk_74a),
        .arst_n(arst_n),
        .bridge_wr(bridge_wr),
        .bridge_rd(bridge_rd),
        .bridge_addr(bridge_addr),
        .reset_n(tb_psst_reset_n),
        .pocket_sst_rd_data(tb_psst_rd_data),
        .pocket_sst_rd_sel(rd_sel),
        .savestate_start(savestate_start),
        .pocket_sst_start_ack(savestate_start_ack),
        .pocket_sst_start_busy(tb_psst_start_busy),
        .pocket_sst_start_ok(tb_psst_start_ok),
        .pocket_sst_start_err(tb_psst_start_err),
        .savestate_load(savestate_load),
        .pocket_sst_load_ack(savestate_load_ack),
        .pocket_sst_load_busy(tb_psst_load_busy),
        .pocket_sst_load_ok(tb_psst_load_ok),
        .pocket_sst_load_err(tb_psst_load_err)
    );

    logic cke;
    logic [12:0] a;
    logic [1:0] ba;
    logic [1:0] dqm;
    logic ras_n, cas_n, we_n;
    logic [15:0] dq_c2m, dq_m2c;
    logic dq_oe;
    logic [31:0] refreshes, sref_clocks;

    /* verilator lint_off PINCONNECTEMPTY */
    pocket_sdram sdram (
        .clk(clk_sys),
        .pll_locked(1'b1),
        .rd_pend(rd_pend),
        .rd_addr(rd_addr),
        .pocket_sdram_rword(rword),
        .pocket_sdram_rvalid(rvalid),
        .w_avail(1'b0),
        .w_addr(25'd0),
        .w_data(16'd0),
        .w_mask(2'b00),
        .pocket_sdram_wtake(),
        .line_pend(1'b0),
        .line_we(1'b0),
        .line_addr(22'd0),
        .line_wdata(128'd0),
        .pocket_sdram_line_rdata(),
        .pocket_sdram_line_done(),
        .pocket_sdram_ready(tb_psst_sdram_ready),
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
    /* verilator lint_on PINCONNECTEMPTY */

    sdram_model chip (
        .clk(~clk_sys),
        .rst_n(arst_n),
        .cke(cke),
        .a(a),
        .ba(ba),
        .ras_n(ras_n),
        .cas_n(cas_n),
        .we_n(we_n),
        .dqm(dqm),
        .dq_in(dq_c2m),
        .dq_oe(dq_oe),
        .dq_out(dq_m2c),
        .sdram_model_refreshes(refreshes),
        .sdram_model_sref_clocks(sref_clocks)
    );

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_tb_psst;
    always_comb unused_tb_psst = ^{rd_sel, refreshes, sref_clocks};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
