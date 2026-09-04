/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine and its adapters: everything between the APF shell and
 * rp6502 that is ours to own.
 *
 * The machine's reset is the bridge's run gate under the platform reset.
 * Adapters carrying live streams reset with the machine so a reload
 * re-arms them; the bridge, the store and the I2S engine stay up
 * throughout.
 */

module pocket_core #(
    parameter TCM_INIT_FILE = ""
) (
    /* The machine is stopped by taking its clocks away, which is not
     * this module's to do. It says when it wants them gone and where it
     * is safe to cut -- the 6502 in front of an instruction rather than
     * inside one -- and whoever owns the clock tree does the rest. One
     * control, at the source, instead of an enable in every subsystem
     * that has one. */
    output logic pocket_core_stop_req,
    /* Whether the machine's clocks are being delivered. Whoever owns
     * the clock tree answers. */
    input logic mach_running,

    /* The machine's clock, which stops. Every other clock here does
     * not -- the soft CPU's included: it is halted at its debug port
     * instead, so its clock keeps running. */
    input logic clk_mach,
    input logic clk_rv,

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
    output logic pocket_core_dataslot_flush,
    output logic [15:0] pocket_core_dataslot_id,
    output logic [31:0] pocket_core_dataslot_slotoffset,
    output logic [31:0] pocket_core_dataslot_bridgeaddr,
    output logic [31:0] pocket_core_dataslot_length,
    input logic target_dataslot_done,
    input logic [2:0] target_dataslot_err,

    /* Sleep, which on this platform is a savestate. The command levels
     * are edge-detected and acked in the fabric; everything after the
     * ack is the firmware's. */
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
    /* APF's four controller slots. A slot holds a gamepad, the dock's
     * keyboard as six scan codes across joy and trig, the dock's mouse
     * as a report counter and two movements, or nothing at all — and
     * says which in the top nibble of its key word. The firmware asks;
     * nothing between here and there cares. */
    input logic [3:0][31:0] cont_key,
    input logic [3:0][31:0] cont_joy,
    input logic [3:0][15:0] cont_trig,

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

    /* The board's own SRAM, which holds the 6502's 64 KB. No chip
     * enable: Analogue's port list has none, so the board straps it
     * low and the byte enables are the only deselect there is. */
    output logic [16:0] sram_a,
    output logic [15:0] sram_dq_out,
    output logic sram_dq_oe,
    input logic [15:0] sram_dq_in,
    output logic sram_oe_n,
    output logic sram_we_n,
    output logic sram_ub_n,
    output logic sram_lb_n,

    /* Status toward the shell, and the consoles for the bench. */
    output logic pocket_core_ready,
    output logic [7:0] pocket_core_tx_data,
    output logic pocket_core_tx_valid,
    output logic [7:0] pocket_core_rv_tx_data,
    output logic pocket_core_rv_tx_valid,
    output logic pocket_core_rv_halted
);

    logic run;

    /* The machine's reset, asserted asynchronously and released
     * synchronously, once for every clock it lands in.
     *
     * rst_n and run are levels the platform moves on its own clock, so
     * the gate of them is combinational and can glitch. Worse is the
     * release: this signal is the asynchronous reset of seven thousand
     * nine hundred flops reached through a global network, and an
     * unsynchronised release arrives at each of them at whatever moment
     * the routing gives it. Blocks then leave reset in an order the
     * fitter chose rather than one the design chose — a different order
     * every fit, so a machine that starts wrong in a way no timing
     * report can show and no zero-delay simulation can reproduce. The
     * Design Assistant's rule R101 names it; nothing else in the flow
     * looks.
     *
     * Three domains take it: clk_sys and clk_rv inside the machine, and
     * clk_vid through the video block's own reset port. A release
     * synchronised to one clock is still a race in the others, so each
     * gets its own pair. The first stage is named _s1 so pocket.sdc's
     * standing rule cuts the asynchronous arrival at it. */
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
    /* The Pocket sends no key events; its keyboard arrives as a report
     * and its pad as state. The machine's event mailbox stays for the
     * testbenches, which is the only thing that fills it. */
    /* The machine still offers its key mailbox; nothing on the Pocket
     * fills it, so nothing here reads whether it is full. */
    logic key_pending;
    logic [3:0][31:0] cont_key_sys, cont_joy_sys;
    logic [3:0][15:0] cont_trig_sys;
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
        .dataslot_update(dataslot_update),
        .reset_n(reset_n),
        .pocket_bridge_dt_addr(bridge_dt_addr),
        .pocket_bridge_dt_busy(dt_busy),
        .datatable_q(datatable_q),
        .cont_key(cont_key),
        .cont_joy(cont_joy),
        .cont_trig(cont_trig),
        .clk_sys(clk_sys),
        .sdram_ready(pocket_core_ready),
        .w_take(w_take),
        .pocket_bridge_w_avail(w_avail),
        .pocket_bridge_w_addr(w_addr),
        .pocket_bridge_w_data(w_data),
        .pocket_bridge_run(run),
        .pocket_bridge_slot_set(slot_set),
        .pocket_bridge_slot_len(slot_len),
        .pocket_bridge_upd_n(upd_n),
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

    logic [15:0] ram_a_addr, ram_b_addr /*verilator public_flat_rd*/;
    logic [7:0] ram_a_wdata, ram_b_wdata, ram_a_rdata, ram_b_rdata;
    logic ram_refill;
    logic ram_a_we, ram_b_we /*verilator public_flat_rd*/,
        ram_b_stb /*verilator public_flat_rd*/,
        ram_b_stall /*verilator public_flat_rd*/, ram_hold;
    /* machine_resb carries the machine's effective reset, which since
     * the restore rework is resb AND the engine's reset hold -- a
     * combinational term. It async-resets the 6502 and the VIA inside
     * the machine and paces port A here synchronously; the lint that
     * dislikes that mix is answered by construction: both sources are
     * single flops and every orchestrated transition is monotonic. */
    /* verilator lint_off SYNCASYNCNET */
    logic machine_phi2_en, machine_resb;
    /* verilator lint_on SYNCASYNCNET */

    /* The machine-clock enable, brought into this clock so port A's
     * launch condition can be trusted while the machine is stopped. */
    (* preserve *) logic run_s1, run_s2;
    initial begin
        run_s1 = 1'b1;
        run_s2 = 1'b1;
    end
    always_ff @(posedge clk_sys) begin
        run_s1 <= mach_running;
        run_s2 <= run_s1;
    end

    /* What the SRAM's port A is masked by, which is not the same
     * question. It has to be high before the gate closes is too early
     * -- that masks live 6502 accesses -- and low again before the
     * clock returns, because the launch condition is phi2_en and the
     * first enable after a resume is legal. Waiting for mach_running
     * to come back is two or three clocks too late, and at eight
     * megahertz the 6502 has a better than even chance of spending one
     * of them, taking a cycle on a byte port A never went and got.
     *
     * So: the same shape the serializer uses for its own array mask.
     * Up once the gate has actually closed -- the request alone leads
     * it -- and down the instant the request goes, which is several of
     * the host's clocks before the gate opens again. */
    logic mach_gated;
    always_comb mach_gated = pocket_core_stop_req && !run_s2;

    pocket_sram sram (
        .clk(clk_sys),
        .run(!mach_gated),
        .refill(ram_refill),
        .phi2_en(machine_phi2_en),
        .cpu_run(machine_resb),
        .a_addr(ram_a_addr),
        .a_wdata(ram_a_wdata),
        .a_we(ram_a_we),
        .pocket_sram_a_rdata(ram_a_rdata),
        .b_addr(ram_b_addr),
        .b_wdata(ram_b_wdata),
        .b_we(ram_b_we),
        .b_stb(ram_b_stb),
        .pocket_sram_b_rdata(ram_b_rdata),
        .pocket_sram_b_stall(ram_b_stall),
        .pocket_sram_hold(ram_hold),
        .pocket_sram_a(sram_a),
        .pocket_sram_dq_out(sram_dq_out),
        .pocket_sram_dq_oe(sram_dq_oe),
        .sram_dq_in(sram_dq_in),
        .pocket_sram_oe_n(sram_oe_n),
        .pocket_sram_we_n(sram_we_n),
        .pocket_sram_ub_n(sram_ub_n),
        .pocket_sram_lb_n(sram_lb_n)
    );

    logic [27:0] host_addr;
    logic host_stb, host_we;
    logic [31:0] host_wdata, host_rdata, file_rdata, sst_rdata;
    logic [31:0] set_tz, set_tz_min, set_tz_sign;
    logic [31:0] set_kb;
    logic [31:0] rtc_epoch_sys;
    logic rtc_valid_sys;

    /* The platform window's second half is what the interact menu has
     * set plus the host's clock, read-only: the machine polls it and
     * applies what changed. Bit 16 picks it and bit 17 the savestate
     * bridge; the file bridge owns everything below both. */
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
                default: set_rdata <= '0;
            endcase
    end
    always_comb
        host_rdata = host_sel_q[1] ? sst_rdata
            : (host_sel_q[0] ? set_rdata : file_rdata);

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

    /* The savestate engine holds the machine; nothing outside walks
     * its state port directly. */
    logic sst_save, sst_ready, sst_rd_sel, sst_word_valid, sst_rd_t;
    logic sst_load, sst_load_done, sst_load_err;
    logic [17:0] sst_rd_idx;
    logic [31:0] sst_word, sst_rd_data;

    /* verilator lint_off PINCONNECTEMPTY */
    wiring #(.TCM_INIT_FILE(TCM_INIT_FILE), .EXT_RAM(1)) machine (
        .clk_mach(clk_mach),
        .mach_running(mach_running),
        .wiring_sst_stop_req(pocket_core_stop_req),
        .sst_tcm_sel(1'b0),
        .sst_tcm_addr(15'd0),
        .sst_tcm_we(1'b0),
        .sst_tcm_wdata(32'd0),
        .wiring_sst_tcm_rdata(),
        .sst_dbg_halt(1'b0),
        .sst_dbg_halt_on_reset(1'b0),
        .sst_dbg_resume(1'b0),
        .wiring_sst_dbg_halted(),
        .sst_dbg_data0(32'd0),
        .wiring_sst_dbg_data0(),
        .wiring_sst_dbg_data0_wen(),
        .sst_dbg_instr(32'd0),
        .sst_dbg_instr_vld(1'b0),
        .wiring_sst_dbg_instr_rdy(),
        .wiring_sst_dbg_ebreak(),
        .wiring_sst_dbg_fault(),
        .sst_save(sst_save),
        .sst_load(sst_load),
        .wiring_sst_load_done(sst_load_done),
        .wiring_sst_load_err(sst_load_err),
        .wiring_sst_ready(sst_ready),
        .sst_rd_idx(sst_rd_idx),
        .sst_rd_t(sst_rd_t),
        .wiring_sst_rdata(sst_word),
        .wiring_sst_rvalid(sst_word_valid),
        .clk_sys(clk_sys),
        .clk_rv(clk_rv),
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
        .stage_stall(stage_stall),
        .stage_rdata(stage_rdata),
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
        .wiring_vid_frame(vid_frame),
        .wiring_ram_a_addr(ram_a_addr),
        .wiring_ram_a_wdata(ram_a_wdata),
        .wiring_ram_a_we(ram_a_we),
        .ram_a_rdata(ram_a_rdata),
        .wiring_ram_b_addr(ram_b_addr),
        .wiring_ram_b_wdata(ram_b_wdata),
        .wiring_ram_b_we(ram_b_we),
        .wiring_ram_b_stb(ram_b_stb),
        .wiring_ram_refill(ram_refill),
        .ram_b_rdata(ram_b_rdata),
        .ram_b_stall(ram_b_stall),
        .ram_hold(ram_hold),
        .wiring_phi2_en(machine_phi2_en),
        .wiring_cpu_run(machine_resb)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    /* The file bridge keeps the platform reset, not the machine's: a
     * command in flight belongs to the host, and a reboot that dropped
     * it would leave the bridge waiting for a core that had forgotten
     * it asked. */
    pocket_file file (
        .clk_sys(clk_sys),
        .stb(host_stb && !host_addr[16] && !host_addr[17]),
        .we(host_we),
        .addr(host_addr),
        .wdata(host_wdata),
        .pocket_file_rdata(file_rdata),
        .w_pending(w_avail),
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
        .pocket_file_flush(pocket_core_dataslot_flush),
        .pocket_file_id(pocket_core_dataslot_id),
        .pocket_file_slotoffset(pocket_core_dataslot_slotoffset),
        .pocket_file_bridgeaddr(pocket_core_dataslot_bridgeaddr),
        .pocket_file_length(pocket_core_dataslot_length),
        .target_dataslot_done(target_dataslot_done),
        .target_dataslot_err(target_dataslot_err)
    );

    /* Like the file bridge, on the platform's reset rather than the
     * machine's: a savestate belongs to the host, and a reboot that
     * dropped one would leave the bridge waiting. */
    pocket_sst sst (
        .clk_sys(clk_sys),
        .stb(host_stb && host_addr[17]),
        .we(host_we),
        .addr(host_addr),
        .wdata(host_wdata),
        .pocket_sst_rdata(sst_rdata),
        .clk_74a(clk_74a),
        .arst_n(arst_n),
        .bridge_wr(bridge_wr),
        .bridge_rd(bridge_rd),
        .bridge_addr(bridge_addr),
        .pocket_sst_rd_data(sst_rd_data),
        .pocket_sst_rd_sel(sst_rd_sel),
        .pocket_sst_save(sst_save),
        .pocket_sst_load(sst_load),
        .sst_load_done(sst_load_done),
        .sst_load_err(sst_load_err),
        .sst_ready(sst_ready),
        .pocket_sst_rd_idx(sst_rd_idx),
        .pocket_sst_rd_t(sst_rd_t),
        .sst_rdata(sst_word),
        .sst_rvalid(sst_word_valid),
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

    /* Two things answer a bridge read, and the blob's window is decoded
     * exactly rather than by the megabyte around it. */
    logic [31:0] file_rd_data;
    always_comb
        pocket_core_bridge_rd_data = sst_rd_sel ? sst_rd_data : file_rd_data;

    pocket_video video (
        .run(run_s2),
        .clk_mach(clk_mach),
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

    /* The I2S keeps the platform reset: both FIFO sides stay one
     * family, and a machine reset just stops the pushes — the shifter
     * repeats the last sample, flat, until the reboot resumes. */
    pocket_i2s i2s (
        .clk_mach(clk_mach),
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
