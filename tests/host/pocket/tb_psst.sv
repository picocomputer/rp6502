/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * pocket_sst behind the core's own core_bridge_cmd, so the host's
 * savestate commands are sent as the Pocket sends them, as writes to
 * 0xF8000000, and their result codes are the ones the Pocket reads.
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

    input logic clk_74a,
    input logic arst_n,
    input logic bridge_wr,
    input logic bridge_rd,
    input logic [31:0] bridge_addr,
    input logic [31:0] bridge_wr_data,
    output logic [31:0] tb_psst_rd_data,
    output logic [31:0] tb_psst_cmd_rd_data,
    output logic tb_psst_reset_n,

    output logic tb_psst_save,
    input logic sst_ready,
    output logic [17:0] tb_psst_rd_idx,
    output logic tb_psst_rd_t,
    input logic [31:0] sst_rdata,
    input logic sst_rvalid,

    output logic tb_psst_load,
    input logic sst_load_done,
    input logic sst_load_err,
    input logic [2:0] sst_load_why,

    output logic tb_psst_start_busy,
    output logic tb_psst_start_ok,
    output logic tb_psst_start_err,
    output logic tb_psst_load_busy,
    output logic tb_psst_load_ok,
    output logic tb_psst_load_err,
    /* How many times each request line rose. */
    output logic [7:0] tb_psst_starts,
    output logic [7:0] tb_psst_loads
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
        .savestate_addr(32'h03E8_0000),
        .savestate_size(32'd300368),
        .savestate_maxloadsize(32'd524288),
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
        .target_dataslot_flush(1'b0),
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

    pocket_sst sst (
        .clk_sys(clk_sys),
        .stb(stb),
        .we(we),
        .addr(addr),
        .wdata(wdata),
        .pocket_sst_rdata(tb_psst_rdata),
        .clk_74a(clk_74a),
        .arst_n(arst_n),
        .bridge_wr(bridge_wr),
        .bridge_rd(bridge_rd),
        .bridge_addr(bridge_addr),
        .reset_n(tb_psst_reset_n),
        .pocket_sst_rd_data(tb_psst_rd_data),
        .pocket_sst_rd_sel(rd_sel),
        .pocket_sst_save(tb_psst_save),
        .sst_ready(sst_ready),
        .pocket_sst_rd_idx(tb_psst_rd_idx),
        .pocket_sst_rd_t(tb_psst_rd_t),
        .sst_rdata(sst_rdata),
        .sst_rvalid(sst_rvalid),
        .pocket_sst_load(tb_psst_load),
        .sst_load_done(sst_load_done),
        .sst_load_err(sst_load_err),
        .sst_load_why(sst_load_why),
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

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_tb_psst;
    always_comb unused_tb_psst = rd_sel;
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
