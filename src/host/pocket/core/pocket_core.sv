/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module pocket_core
    import mem_pkg::*, pocket_version_pkg::*;
(
    input logic clk_a2,
    input logic clk_ph,

    input logic clk_74a,
    input logic clk_sys,
    input logic clk_vid,
    input logic rst_n,
    input logic arst_n,

    input logic bridge_wr,
    input logic [31:0] bridge_addr,
    input logic bridge_rd,
    input logic [31:0] bridge_wr_data,
    input logic dataslot_allcomplete,
    input logic dataslot_update,
    input logic dataslot_requestwrite,
    input logic [15:0] dataslot_requestwrite_id,
    input logic reset_n,
    output logic [9:0] pocket_core_dt_addr,
    input logic [31:0] datatable_q,
    input logic [31:0] rtc_epoch,
    input logic rtc_valid,
    output logic [31:0] pocket_core_bridge_rd_data,
    output logic [31:0] pocket_core_param_struct,
    output logic [31:0] pocket_core_resp_struct,
    output logic pocket_core_dataslot_read,
    output logic pocket_core_dataslot_write,
    output logic pocket_core_dataslot_openfile,
    output logic pocket_core_dataslot_getfile,
    output logic [15:0] pocket_core_dataslot_id,
    output logic [31:0] pocket_core_dataslot_slotoffset,
    output logic [31:0] pocket_core_dataslot_bridgeaddr,
    output logic [31:0] pocket_core_dataslot_length,
    input logic target_dataslot_done,
    input logic [2:0] target_dataslot_err,

    input logic savestate_start,
    output logic pocket_core_savestate_start_ack,
    output logic pocket_core_savestate_start_busy,
    output logic pocket_core_savestate_start_ok,
    output logic pocket_core_savestate_start_err,
    input logic savestate_load,
    output logic pocket_core_savestate_load_ack,
    output logic pocket_core_savestate_load_busy,
    output logic pocket_core_savestate_load_ok,
    output logic pocket_core_savestate_load_err,
    input logic [3:0][31:0] cont_key,
    input logic [3:0][31:0] cont_joy,
    input logic [3:0][15:0] cont_trig,

    output logic [23:0] pocket_core_rgb,
    output logic pocket_core_de,
    output logic pocket_core_skip,
    output logic pocket_core_vs,
    output logic pocket_core_hs,

    output logic pocket_core_mclk,
    output logic pocket_core_dac,
    output logic pocket_core_lrck,

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

    output logic pocket_core_ready,
    output logic [7:0] pocket_core_tx_data,
    output logic pocket_core_tx_valid,
    output logic [7:0] pocket_core_rv_tx_data,
    output logic pocket_core_rv_tx_valid,
    output logic pocket_core_rv_halted
);

    /* The soft CPU's memory starts at this byte address, after the ROM
     * slot: the ROM block, where the host loads firmware.bin as a data
     * slot, and the RAM block after it, MEM_BYTES in all. They are the
     * only SDRAM its cache holds. The file windows and the Get File scratch follow them, and
     * the savestate window, SST_WINDOW bytes at SST_BASE, is last and runs
     * to the end of the SDRAM. The soft CPU reaches all three through the
     * staging store. stage_map_gate.py checks these against data.json and
     * mmio.h. */
    localparam logic [31:0] MEM_BASE = 32'h03E0_0000;
    localparam logic [31:0] MEM_BYTES = 32'h0005_0000;
    localparam logic [31:0] SST_BASE = 32'h03EC_1000;
    localparam logic [31:0] SST_WINDOW = 32'h0013_F000;

    logic run;

    logic mrst_raw_n;
    always_comb mrst_raw_n = rst_n && run;

    logic mrst_sys_s1, mrst_sys_n;
    always_ff @(posedge clk_sys or negedge mrst_raw_n) begin
        if (!mrst_raw_n) begin
            mrst_sys_s1 <= 1'b0;
            mrst_sys_n <= 1'b0;
        end else begin
            mrst_sys_s1 <= 1'b1;
            mrst_sys_n <= mrst_sys_s1;
        end
    end

    logic slot_set;
    logic [7:0] upd_n;
    logic rom_coming;
    logic key_pending;
    logic [3:0][31:0] cont_key_sys, cont_joy_sys;
    logic [3:0][15:0] cont_trig_sys;
    logic [31:0] slot_len;

    logic bw_avail, w_queued, bw_take;
    logic [24:0] bw_addr;
    logic [15:0] bw_data;

    logic [9:0] bridge_dt_addr, file_dt_addr;
    logic dt_busy, file_dt_req;
    always_comb pocket_core_dt_addr = file_dt_req && !dt_busy
        ? file_dt_addr : bridge_dt_addr;

    pocket_bridge #(
        .MEM_BASE(MEM_BASE),
        .MEM_BYTES(MEM_BYTES)
    ) bridge (
        .clk_74a(clk_74a),
        .arst_n(arst_n),
        .bridge_wr(bridge_wr),
        .bridge_addr(bridge_addr),
        .bridge_wr_data(bridge_wr_data),
        .dataslot_allcomplete(dataslot_allcomplete),
        .dataslot_update(dataslot_update),
        .dataslot_requestwrite(dataslot_requestwrite),
        .dataslot_requestwrite_id(dataslot_requestwrite_id),
        .reset_n(reset_n),
        .pocket_bridge_dt_addr(bridge_dt_addr),
        .pocket_bridge_dt_busy(dt_busy),
        .datatable_q(datatable_q),
        .cont_key(cont_key),
        .cont_joy(cont_joy),
        .cont_trig(cont_trig),
        .clk_sys(clk_sys),
        .sdram_ready(pocket_core_ready),
        .w_take(bw_take),
        .pocket_bridge_w_avail(bw_avail),
        .pocket_bridge_w_queued(w_queued),
        .pocket_bridge_w_addr(bw_addr),
        .pocket_bridge_w_data(bw_data),
        .pocket_bridge_run(run),
        .pocket_bridge_slot_set(slot_set),
        .pocket_bridge_slot_len(slot_len),
        .pocket_bridge_upd_n(upd_n),
        .pocket_bridge_rom_coming(rom_coming),
        .pocket_bridge_cont_key(cont_key_sys),
        .pocket_bridge_cont_joy(cont_joy_sys),
        .pocket_bridge_cont_trig(cont_trig_sys),
        .pocket_bridge_set_tz(set_tz),
        .pocket_bridge_set_tz_min(set_tz_min),
        .pocket_bridge_set_tz_sign(set_tz_sign),
        .pocket_bridge_set_kb(set_kb),
        .rtc_epoch(rtc_epoch),
        .rtc_valid(rtc_valid),
        .pocket_bridge_rtc_epoch(rtc_epoch_sys),
        .pocket_bridge_rtc_valid(rtc_valid_sys)
    );

    /* The SDRAM's read client is the soft CPU's staging store, and
     * pocket_sst's while it fetches a word of the savestate for the
     * host, which stalls the soft CPU's read meanwhile. */
    logic stage_pend, stage_we;
    logic [27:0] stage_addr;
    logic [31:0] stage_word, stage_wdata;
    logic [3:0] stage_wstrb;
    logic stage_rvalid;
    logic stage_stall;
    logic sst_rd_pend;
    logic [23:0] sst_rd_addr;

    /* The soft CPU's writes to the staging store go to the SDRAM's write
     * client a halfword at a time, and a half its strobes leave out is
     * skipped. The bridge's writes go first, because its queue cannot
     * make the host wait. A staging access waits while a write of the
     * soft CPU's is still on its way, and the controller drops its
     * look-ahead line when it takes a write, so a read after a write is
     * given its bytes. sw_strb holds the bytes still to write. */
    logic [23:0] sw_addr;
    logic [31:0] sw_data;
    logic [3:0] sw_strb;
    logic sw_busy, sw_hi;
    always_comb begin
        sw_busy = sw_strb != 4'd0;
        sw_hi = sw_strb[1:0] == 2'd0;
        stage_stall = stage_pend && (sw_busy
            || (!stage_we && (sst_rd_pend || !stage_rvalid)));
    end

    /* The controller takes a write a clock after it chooses it, so the
     * choice is kept for the take. */
    logic w_avail, w_take, w_cpu, w_cpu_q;
    logic [24:0] w_addr;
    logic [15:0] w_data;
    logic [1:0] w_mask;
    always_comb begin
        w_cpu = !bw_avail;
        w_avail = bw_avail || sw_busy;
        w_addr = w_cpu ? {sw_addr, sw_hi} : bw_addr;
        w_data = !w_cpu ? bw_data : sw_hi ? sw_data[31:16] : sw_data[15:0];
        w_mask = !w_cpu ? 2'b11 : sw_hi ? sw_strb[3:2] : sw_strb[1:0];
        bw_take = w_take && !w_cpu_q;
    end
    initial begin
        sw_addr = '0;
        sw_data = '0;
        sw_strb = '0;
        w_cpu_q = 1'b0;
    end
    always_ff @(posedge clk_sys) begin
        w_cpu_q <= w_cpu;
        if (w_take && w_cpu_q) begin
            if (sw_hi)
                sw_strb[3:2] <= 2'd0;
            else
                sw_strb[1:0] <= 2'd0;
        end
        if (stage_pend && stage_we && !stage_stall) begin
            sw_addr <= stage_addr[25:2];
            sw_data <= stage_wdata;
            sw_strb <= stage_wstrb;
        end
    end

    logic line_pend, line_we;
    logic [MEM_AW-3:0] line_addr;
    logic [127:0] line_wdata, line_rdata;
    logic line_done;

    pocket_sdram sdram (
        .clk(clk_sys),
        .pll_locked(rst_n),
        .rd_pend((stage_pend && !stage_we) || sst_rd_pend),
        .rd_addr(sst_rd_pend ? sst_rd_addr : stage_addr[25:2]),
        .pocket_sdram_rword(stage_word),
        .pocket_sdram_rvalid(stage_rvalid),
        .w_avail(w_avail),
        .w_addr(w_addr),
        .w_data(w_data),
        .w_mask(w_mask),
        .pocket_sdram_wtake(w_take),
        .line_pend(line_pend),
        .line_we(line_we),
        .line_addr({MEM_BASE[25:MEM_AW+2], line_addr}),
        .line_wdata(line_wdata),
        .pocket_sdram_line_rdata(line_rdata),
        .pocket_sdram_line_done(line_done),
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
    logic [31:0] host_wdata, host_rdata, file_rdata, sst_rdata;
    logic [31:0] set_tz, set_tz_min, set_tz_sign;
    logic [31:0] set_kb;
    logic [31:0] rtc_epoch_sys;
    logic rtc_valid_sys;

    logic [31:0] set_rdata;
    initial set_rdata = '0;
    always_ff @(posedge clk_sys) begin
        if (host_stb)
            case (host_addr[4:2])
                3'd0: set_rdata <= set_kb;
                3'd2: set_rdata <= set_tz;
                3'd3: set_rdata <= rtc_epoch_sys;
                3'd4: set_rdata <= {31'd0, rtc_valid_sys};
                3'd5: set_rdata <= set_tz_min;
                3'd6: set_rdata <= set_tz_sign;
                3'd7: set_rdata <= {31'd0, rom_coming};
                default: set_rdata <= '0;
            endcase
    end
    /* The register layout this bitstream shares with its firmware,
     * which gen/version.py states, at 0x80030000. The firmware reads it
     * before it uses any other register, so this address never moves. */
    logic [31:0] ver_rdata;
    initial ver_rdata = '0;
    always_ff @(posedge clk_sys)
        if (host_stb)
            ver_rdata <= REG_VERSION;

    always_comb
        case (host_sel_q)
            2'd0: host_rdata = file_rdata;
            2'd1: host_rdata = set_rdata;
            2'd2: host_rdata = sst_rdata;
            default: host_rdata = ver_rdata;
        endcase

    logic [1:0] host_sel_q;
    initial host_sel_q = '0;
    always_ff @(posedge clk_sys)
        if (host_stb)
            host_sel_q <= host_addr[17:16];

    logic [15:0] vid_pixel;
    logic vid_de, vid_frame;
    logic [2:0] vid_canvas;
    logic signed [15:0] aud_l, aud_r;
    logic aud_valid;
    logic rx_taken;
    logic [31:0] rv_exit_code;
    logic [9:0] scanline;

    logic sst_rd_sel;
    logic [31:0] sst_rd_data;

    wiring machine (
        .rom_stop(rom_coming),
        .wiring_line_pend(line_pend),
        .wiring_line_we(line_we),
        .wiring_line_addr(line_addr),
        .wiring_line_wdata(line_wdata),
        .line_done(line_done),
        .line_rdata(line_rdata),
        .clk_sys(clk_sys),
        .clk_a2(clk_a2),
        .clk_ph(clk_ph),
        .rst_n(mrst_sys_n),
        .wiring_tx_data(pocket_core_tx_data),
        .wiring_tx_valid(pocket_core_tx_valid),
        .rx_valid(1'b0),
        .rx_data(8'h00),
        .wiring_rx_taken(rx_taken),
        .wiring_rv_tx_data(pocket_core_rv_tx_data),
        .wiring_rv_tx_valid(pocket_core_rv_tx_valid),
        .wiring_rv_halted(pocket_core_rv_halted),
        .wiring_rv_exit_code(rv_exit_code),
        .wiring_stage_addr(stage_addr),
        .wiring_stage_pend(stage_pend),
        .wiring_stage_we(stage_we),
        .wiring_stage_wdata(stage_wdata),
        .wiring_stage_wstrb(stage_wstrb),
        .stage_stall(stage_stall),
        .stage_word(stage_word),
        .wiring_host_addr(host_addr),
        .wiring_host_stb(host_stb),
        .wiring_host_we(host_we),
        .wiring_host_wdata(host_wdata),
        .host_rdata(host_rdata),
        .slot_set(slot_set),
        .slot_len(slot_len),
        .upd_n(upd_n),
        .key_set(1'b0),
        .key_code(9'd0),
        .cont_key(cont_key_sys),
        .cont_joy(cont_joy_sys),
        .cont_trig(cont_trig_sys),
        .wiring_key_pending(key_pending),
        .wiring_vid_pixel(vid_pixel),
        .wiring_vid_de(vid_de),
        .wiring_vid_canvas(vid_canvas),
        .wiring_aud_l(aud_l),
        .wiring_aud_r(aud_r),
        .wiring_aud_valid(aud_valid),
        .wiring_scanline(scanline),
        .wiring_vid_frame(vid_frame)
    );

    pocket_file file (
        .clk_sys(clk_sys),
        .stb(host_stb && !host_addr[16] && !host_addr[17]),
        .we(host_we),
        .addr(host_addr),
        .wdata(host_wdata),
        .pocket_file_rdata(file_rdata),
        .w_pending(w_queued),
        .clk_74a(clk_74a),
        .arst_n(arst_n),
        .bridge_addr(bridge_addr),
        .bridge_rd(bridge_rd),
        .bridge_wr(bridge_wr),
        .pocket_file_rd_data(file_rd_data),
        .pocket_file_param_struct(pocket_core_param_struct),
        .pocket_file_resp_struct(pocket_core_resp_struct),
        .pocket_file_dt_req(file_dt_req),
        .pocket_file_dt_addr(file_dt_addr),
        .datatable_q(datatable_q),
        .dt_busy(dt_busy),
        .pocket_file_read(pocket_core_dataslot_read),
        .pocket_file_write(pocket_core_dataslot_write),
        .pocket_file_openfile(pocket_core_dataslot_openfile),
        .pocket_file_getfile(pocket_core_dataslot_getfile),
        .pocket_file_id(pocket_core_dataslot_id),
        .pocket_file_slotoffset(pocket_core_dataslot_slotoffset),
        .pocket_file_bridgeaddr(pocket_core_dataslot_bridgeaddr),
        .pocket_file_length(pocket_core_dataslot_length),
        .target_dataslot_done(target_dataslot_done),
        .target_dataslot_err(target_dataslot_err)
    );

    pocket_sst #(
        .BLOB_BASE(SST_BASE),
        .BLOB_WINDOW(SST_WINDOW)
    ) sst (
        .clk_sys(clk_sys),
        .stb(host_stb && host_addr[17:16] == 2'd2),
        .we(host_we),
        .addr(host_addr),
        .wdata(host_wdata),
        .pocket_sst_rdata(sst_rdata),
        .w_queued(w_queued),
        .pocket_sst_rd_pend(sst_rd_pend),
        .pocket_sst_rd_addr(sst_rd_addr),
        .rd_rvalid(stage_rvalid),
        .rd_rword(stage_word),
        .clk_74a(clk_74a),
        .arst_n(arst_n),
        .bridge_wr(bridge_wr),
        .bridge_rd(bridge_rd),
        .bridge_addr(bridge_addr),
        .reset_n(reset_n),
        .pocket_sst_rd_data(sst_rd_data),
        .pocket_sst_rd_sel(sst_rd_sel),
        .savestate_start(savestate_start),
        .pocket_sst_start_ack(pocket_core_savestate_start_ack),
        .pocket_sst_start_busy(pocket_core_savestate_start_busy),
        .pocket_sst_start_ok(pocket_core_savestate_start_ok),
        .pocket_sst_start_err(pocket_core_savestate_start_err),
        .savestate_load(savestate_load),
        .pocket_sst_load_ack(pocket_core_savestate_load_ack),
        .pocket_sst_load_busy(pocket_core_savestate_load_busy),
        .pocket_sst_load_ok(pocket_core_savestate_load_ok),
        .pocket_sst_load_err(pocket_core_savestate_load_err)
    );

    logic [31:0] file_rd_data;
    always_comb
        pocket_core_bridge_rd_data = sst_rd_sel ? sst_rd_data : file_rd_data;

    pocket_video video (
        .clk_sys(clk_sys),
        .vid_pixel(vid_pixel),
        .vid_de(vid_de),
        .vid_frame(vid_frame),
        .vid_canvas(vid_canvas),
        .clk_vid(clk_vid),
        .pocket_video_rgb(pocket_core_rgb),
        .pocket_video_de(pocket_core_de),
        .pocket_video_skip(pocket_core_skip),
        .pocket_video_vs(pocket_core_vs),
        .pocket_video_hs(pocket_core_hs)
    );

    pocket_i2s i2s (
        .clk_sys(clk_sys),
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
                                       stage_addr[27:26], stage_addr[1:0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
