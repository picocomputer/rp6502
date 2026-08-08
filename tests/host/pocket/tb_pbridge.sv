/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Bench top: the bridge feeding the staging store, the behavioral
 * chip behind it, the core_bridge_cmd signals played from C++.
 */

module tb_pbridge (
    input logic clk_74a,
    input logic arst_n,
    input logic bridge_wr,
    input logic [31:0] bridge_addr,
    input logic [31:0] bridge_wr_data,
    input logic dataslot_allcomplete,
    input logic dataslot_update,
    output logic [7:0] tb_pbridge_upd_n,
    input logic reset_n,
    output logic [9:0] tb_pbridge_dt_addr,
    input logic [31:0] datatable_q,
    input logic [31:0] cont1_key,
    input logic [31:0] cont1_joy,
    input logic [15:0] cont1_trig,
    input logic [31:0] cont3_key,
    input logic [31:0] cont3_joy,
    input logic [15:0] cont3_trig,
    input logic [31:0] cont4_key,
    input logic [31:0] cont4_joy,
    input logic [15:0] cont4_trig,

    input logic clk_sys,
    input logic rst_n,
    output logic tb_pbridge_run,
    output logic tb_pbridge_slot_set,
    output logic [31:0] tb_pbridge_slot_len,
    output logic [31:0] tb_pbridge_pad_key,
    output logic [31:0] tb_pbridge_pad_joy,
    output logic [15:0] tb_pbridge_pad_trig,
    output logic [31:0] tb_pbridge_kbd_key,
    output logic [31:0] tb_pbridge_kbd_joy,
    output logic [15:0] tb_pbridge_kbd_trig,
    output logic [31:0] tb_pbridge_mou_key,
    output logic [31:0] tb_pbridge_mou_joy,
    output logic [15:0] tb_pbridge_mou_trig,
    output logic tb_pbridge_ready,

    input logic rd_pend,
    input logic [24:0] rd_addr,
    output logic [15:0] tb_pbridge_rdata,
    output logic tb_pbridge_rvalid
);

    logic dt_busy;
    logic [31:0] set_tz, rtc_epoch_s;
    logic [31:0] set_tz_min, set_tz_sign;
    logic rtc_valid_s;
    logic w_avail, w_take;
    logic [24:0] w_addr;
    logic [15:0] w_data;

    pocket_bridge bridge (
        .clk_74a(clk_74a),
        .arst_n(arst_n),
        .bridge_wr(bridge_wr),
        .bridge_addr(bridge_addr),
        .bridge_wr_data(bridge_wr_data),
        .dataslot_allcomplete(dataslot_allcomplete),
        .dataslot_update(dataslot_update),
        .pocket_bridge_upd_n(tb_pbridge_upd_n),
        .reset_n(reset_n),
        .pocket_bridge_dt_addr(tb_pbridge_dt_addr),
        .pocket_bridge_dt_busy(dt_busy),
        .datatable_q(datatable_q),
        .cont1_key(cont1_key),
        .cont1_joy(cont1_joy),
        .cont1_trig(cont1_trig),
        .cont3_key(cont3_key),
        .cont3_joy(cont3_joy),
        .cont3_trig(cont3_trig),
        .cont4_key(cont4_key),
        .cont4_joy(cont4_joy),
        .cont4_trig(cont4_trig),
        .clk_sys(clk_sys),
        .sdram_ready(tb_pbridge_ready),
        .w_take(w_take),
        .pocket_bridge_w_avail(w_avail),
        .pocket_bridge_w_addr(w_addr),
        .pocket_bridge_w_data(w_data),
        .pocket_bridge_run(tb_pbridge_run),
        .pocket_bridge_slot_set(tb_pbridge_slot_set),
        .pocket_bridge_slot_len(tb_pbridge_slot_len),
        .pocket_bridge_pad_key(tb_pbridge_pad_key),
        .pocket_bridge_pad_joy(tb_pbridge_pad_joy),
        .pocket_bridge_pad_trig(tb_pbridge_pad_trig),
        .pocket_bridge_kbd_key(tb_pbridge_kbd_key),
        .pocket_bridge_kbd_joy(tb_pbridge_kbd_joy),
        .pocket_bridge_kbd_trig(tb_pbridge_kbd_trig),
        .pocket_bridge_mou_key(tb_pbridge_mou_key),
        .pocket_bridge_mou_joy(tb_pbridge_mou_joy),
        .pocket_bridge_mou_trig(tb_pbridge_mou_trig),
        .pocket_bridge_set_tz(set_tz),
        .pocket_bridge_set_tz_min(set_tz_min),
        .pocket_bridge_set_tz_sign(set_tz_sign),
        .rtc_epoch(32'd0),
        .rtc_valid(1'b0),
        .pocket_bridge_rtc_epoch(rtc_epoch_s),
        .pocket_bridge_rtc_valid(rtc_valid_s)
    );

    logic cke;
    logic [12:0] a;
    logic [1:0] ba;
    logic [1:0] dqm;
    logic ras_n, cas_n, we_n;
    logic [15:0] dq_c2m, dq_m2c;
    logic dq_oe;

    pocket_sdram ctrl (
        .clk(clk_sys),
        .rd_pend(rd_pend),
        .rd_addr(rd_addr),
        .pocket_sdram_rdata(tb_pbridge_rdata),
        .pocket_sdram_rvalid(tb_pbridge_rvalid),
        .w_avail(w_avail),
        .w_addr(w_addr),
        .w_data(w_data),
        .pocket_sdram_wtake(w_take),
        .pocket_sdram_ready(tb_pbridge_ready),
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

    logic [31:0] refreshes;
    logic [31:0] sref_clocks;
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
    logic unused_tb_pbridge;
    always_comb unused_tb_pbridge = ^{dqm, refreshes, sref_clocks, dt_busy, set_tz, set_tz_min,
                                      set_tz_sign, rtc_epoch_s,
                                      rtc_valid_s};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
