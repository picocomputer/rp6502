/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine and its four adapters, assembled: everything between
 * the APF shell and rp6502 that is ours to own. The shell — or the
 * bench — supplies clocks, the bridge write stream, and the
 * core_bridge_cmd signals; this module turns them into a running
 * machine with its beam on the scaler, its samples on the codec, and
 * its staged ROM behind the SDRAM. The machine's reset is the
 * bridge's run gate under the platform reset; the adapters that carry
 * live streams (video capture, sample feed) reset with the machine so
 * a reload re-arms them, while the bridge, the store, and the I2S
 * engine stay up throughout.
 */

module pocket_core #(
    parameter TCM_INIT_FILE = ""
) (
    /* Clocks and the platform reset. */
    input logic clk_74a,
    input logic clk_sys,
    /* Half clk_sys, rising with it; see pocket_pll. */
    input logic clk_rv,
    input logic clk_vid,
    input logic rst_n,
    input logic arst_n,

    /* core_bridge_cmd's signals, bridge domain. */
    input logic bridge_wr,
    input logic [31:0] bridge_addr,
    input logic [31:0] bridge_wr_data,
    input logic dataslot_allcomplete,
    input logic reset_n,
    output logic [9:0] pocket_core_dt_addr,
    input logic [31:0] datatable_q,
    /* The file bridge: the host's answer to a target command, and the
     * word it reads back out of us. */
    output logic [31:0] pocket_core_bridge_rd_data,
    output logic [31:0] pocket_core_param_struct,
    output logic pocket_core_dataslot_read,
    output logic pocket_core_dataslot_write,
    output logic pocket_core_dataslot_openfile,
    output logic [15:0] pocket_core_dataslot_id,
    output logic [31:0] pocket_core_dataslot_slotoffset,
    output logic [31:0] pocket_core_dataslot_bridgeaddr,
    output logic [31:0] pocket_core_dataslot_length,
    input logic target_dataslot_done,
    input logic [2:0] target_dataslot_err,
    input logic [31:0] cont1_key,
    input logic [31:0] cont1_joy,
    input logic [15:0] cont1_trig,
    /* The dock puts its keyboard on the third slot: six scan codes
     * across joy and trig, the modifiers in key. */
    input logic [31:0] cont3_key,
    input logic [31:0] cont3_joy,
    input logic [15:0] cont3_trig,
    /* The dock puts its mouse on the fourth: a report counter, the
     * buttons, and the two relative movements. */
    input logic [31:0] cont4_key,
    input logic [31:0] cont4_joy,
    input logic [15:0] cont4_trig,

    /* The scaler. */
    output logic [23:0] pocket_core_rgb,
    output logic pocket_core_de,
    output logic pocket_core_skip,
    output logic pocket_core_vs,
    output logic pocket_core_hs,

    /* The codec. */
    output logic pocket_core_mclk,
    output logic pocket_core_dac,
    output logic pocket_core_lrck,

    /* The SDRAM pads. */
    output logic dram_cke,
    output logic [12:0] dram_a,
    output logic [1:0] dram_ba,
    output logic [1:0] dram_dqm,
    output logic dram_ras_n,
    output logic dram_cas_n,
    output logic dram_we_n,
    output logic [15:0] dram_dq_out,
    output logic dram_dq_oe,
    input logic [15:0] dram_dq_in,

    /* Status toward the shell, and the consoles for the bench. */
    output logic pocket_core_ready,
    output logic [7:0] pocket_core_tx_data,
    output logic pocket_core_tx_valid,
    output logic [7:0] pocket_core_rv_tx_data,
    output logic pocket_core_rv_tx_valid,
    output logic pocket_core_rv_halted
);

    logic run;
    logic mrst_n;
    always_comb mrst_n = rst_n && run;

    logic slot_set;
    /* The Pocket sends no key events; its keyboard arrives as a report
     * and its pad as state. The machine's event mailbox stays for the
     * testbenches, which is the only thing that fills it. */
    /* The machine still offers its key mailbox; nothing on the Pocket
     * fills it, so nothing here reads whether it is full. */
    logic key_pending;
    logic [31:0] pad_key, pad_joy, kbd_key, kbd_joy, mou_key, mou_joy;
    logic [15:0] pad_trig, kbd_trig, mou_trig;
    logic [31:0] slot_len;

    logic w_avail, w_take;
    logic [24:0] w_addr;
    logic [15:0] w_data;

    logic [9:0] bridge_dt_addr, file_dt_addr;
    logic dt_busy, file_dt_req;
    always_comb pocket_core_dt_addr = file_dt_req && !dt_busy
        ? file_dt_addr : bridge_dt_addr;

    pocket_bridge bridge (
        .clk_74a(clk_74a),
        .arst_n(arst_n),
        .bridge_wr(bridge_wr),
        .bridge_addr(bridge_addr),
        .bridge_wr_data(bridge_wr_data),
        .dataslot_allcomplete(dataslot_allcomplete),
        .reset_n(reset_n),
        .pocket_bridge_dt_addr(bridge_dt_addr),
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
        .rst_n(rst_n),
        .sdram_ready(pocket_core_ready),
        .w_take(w_take),
        .pocket_bridge_w_avail(w_avail),
        .pocket_bridge_w_addr(w_addr),
        .pocket_bridge_w_data(w_data),
        .pocket_bridge_run(run),
        .pocket_bridge_slot_set(slot_set),
        .pocket_bridge_slot_len(slot_len),
        .pocket_bridge_pad_key(pad_key),
        .pocket_bridge_pad_joy(pad_joy),
        .pocket_bridge_pad_trig(pad_trig),
        .pocket_bridge_kbd_key(kbd_key),
        .pocket_bridge_kbd_joy(kbd_joy),
        .pocket_bridge_kbd_trig(kbd_trig),
        .pocket_bridge_mou_key(mou_key),
        .pocket_bridge_mou_joy(mou_joy),
        .pocket_bridge_mou_trig(mou_trig)
    );

    /* Staging: the machine's byte fetch against the halfword store. */
    logic stage_pend;
    logic [27:0] stage_addr;
    logic [15:0] stage_half;
    logic stage_rvalid;
    logic [7:0] stage_rdata;
    logic stage_stall;
    always_comb begin
        stage_rdata = stage_addr[0] ? stage_half[15:8] : stage_half[7:0];
        stage_stall = stage_pend && !stage_rvalid;
    end

    pocket_sdram sdram (
        .clk(clk_sys),
        .rst_n(rst_n),
        .rd_pend(stage_pend),
        .rd_addr(stage_addr[25:1]),
        .pocket_sdram_rdata(stage_half),
        .pocket_sdram_rvalid(stage_rvalid),
        .w_avail(w_avail),
        .w_addr(w_addr),
        .w_data(w_data),
        .pocket_sdram_wtake(w_take),
        .pocket_sdram_ready(pocket_core_ready),
        .dram_cke(dram_cke),
        .dram_a(dram_a),
        .dram_ba(dram_ba),
        .dram_dqm(dram_dqm),
        .dram_ras_n(dram_ras_n),
        .dram_cas_n(dram_cas_n),
        .dram_we_n(dram_we_n),
        .dram_dq_out(dram_dq_out),
        .dram_dq_oe(dram_dq_oe),
        .dram_dq_in(dram_dq_in)
    );

    logic [27:0] host_addr;
    logic host_stb, host_we;
    logic [31:0] host_wdata, host_rdata;

    logic [15:0] vid_pixel;
    logic vid_de, vid_frame;
    logic [9:0] aud_l, aud_r;
    logic aud_valid;
    logic rx_taken;
    logic [31:0] rv_exit_code;
    logic [9:0] scanline;

    rp6502 #(.TCM_INIT_FILE(TCM_INIT_FILE)) machine (
        .clk_sys(clk_sys),
        .clk_rv(clk_rv),
        .rst_n(mrst_n),
        .rp6502_tx_data(pocket_core_tx_data),
        .rp6502_tx_valid(pocket_core_tx_valid),
        .rx_valid(1'b0),
        .rx_data(8'h00),
        .rp6502_rx_taken(rx_taken),
        .rp6502_rv_tx_data(pocket_core_rv_tx_data),
        .rp6502_rv_tx_valid(pocket_core_rv_tx_valid),
        .rp6502_rv_halted(pocket_core_rv_halted),
        .rp6502_rv_exit_code(rv_exit_code),
        .rp6502_stage_addr(stage_addr),
        .rp6502_stage_pend(stage_pend),
        .stage_stall(stage_stall),
        .stage_rdata(stage_rdata),
        .rp6502_host_addr(host_addr),
        .rp6502_host_stb(host_stb),
        .rp6502_host_we(host_we),
        .rp6502_host_wdata(host_wdata),
        .host_rdata(host_rdata),
        .slot_set(slot_set),
        .slot_len(slot_len),
        .key_set(1'b0),
        .key_code(9'd0),
        .pad_key(pad_key),
        .pad_joy(pad_joy),
        .pad_trig(pad_trig),
        .kbd_key(kbd_key),
        .kbd_joy(kbd_joy),
        .kbd_trig(kbd_trig),
        .mou_key(mou_key),
        .mou_joy(mou_joy),
        .mou_trig(mou_trig),
        .rp6502_key_pending(key_pending),
        .rp6502_vid_pixel(vid_pixel),
        .rp6502_vid_de(vid_de),
        .rp6502_aud_l(aud_l),
        .rp6502_aud_r(aud_r),
        .rp6502_aud_valid(aud_valid),
        .rp6502_scanline(scanline),
        .rp6502_vid_frame(vid_frame)
    );

    /* The file bridge keeps the platform reset, not the machine's: a
     * command in flight belongs to the host, and a reboot that dropped
     * it would leave the bridge waiting for a core that had forgotten
     * it asked. */
    pocket_file file (
        .clk_sys(clk_sys),
        .rst_n(rst_n),
        .stb(host_stb),
        .we(host_we),
        .addr(host_addr),
        .wdata(host_wdata),
        .pocket_file_rdata(host_rdata),
        .w_pending(w_avail),
        .clk_74a(clk_74a),
        .arst_n(arst_n),
        .bridge_addr(bridge_addr),
        .pocket_file_rd_data(pocket_core_bridge_rd_data),
        .pocket_file_param_struct(pocket_core_param_struct),
        .pocket_file_dt_req(file_dt_req),
        .pocket_file_dt_addr(file_dt_addr),
        .datatable_q(datatable_q),
        .dt_busy(dt_busy),
        .pocket_file_read(pocket_core_dataslot_read),
        .pocket_file_write(pocket_core_dataslot_write),
        .pocket_file_openfile(pocket_core_dataslot_openfile),
        .pocket_file_id(pocket_core_dataslot_id),
        .pocket_file_slotoffset(pocket_core_dataslot_slotoffset),
        .pocket_file_bridgeaddr(pocket_core_dataslot_bridgeaddr),
        .pocket_file_length(pocket_core_dataslot_length),
        .target_dataslot_done(target_dataslot_done),
        .target_dataslot_err(target_dataslot_err)
    );

    pocket_video video (
        .clk_sys(clk_sys),
        .rst_n(mrst_n),
        .vid_pixel(vid_pixel),
        .vid_de(vid_de),
        .vid_frame(vid_frame),
        .clk_vid(clk_vid),
        .vrst_n(mrst_n),
        .pocket_video_rgb(pocket_core_rgb),
        .pocket_video_de(pocket_core_de),
        .pocket_video_skip(pocket_core_skip),
        .pocket_video_vs(pocket_core_vs),
        .pocket_video_hs(pocket_core_hs)
    );

    /* The I2S keeps the platform reset: both FIFO sides stay one
     * family, and a machine reset just stops the pushes — the shifter
     * repeats the last sample, flat, until the reboot resumes. */
    pocket_i2s i2s (
        .clk_sys(clk_sys),
        .rst_n(rst_n),
        .aud_l(aud_l),
        .aud_r(aud_r),
        .aud_valid(aud_valid),
        .clk_74a(clk_74a),
        .arst_n(arst_n),
        .pocket_i2s_mclk(pocket_core_mclk),
        .pocket_i2s_dac(pocket_core_dac),
        .pocket_i2s_lrck(pocket_core_lrck)
    );

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_core_key;
    always_comb unused_pocket_core_key = key_pending;
    logic unused_pocket_core;
    always_comb unused_pocket_core = ^{rx_taken, rv_exit_code, scanline,
                                       stage_addr[27:26]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
