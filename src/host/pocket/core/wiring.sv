/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module wiring
    import timing_pkg::*, tcm_pkg::*;
#(
    parameter TCM_INIT_FILE = "",
    parameter int SYS_KHZ = 50400,
    parameter bit EXT_RAM = 0
) (
    input logic clk_sys,
    input logic clk_mach,
    /* clk_rv runs at half the rate of clk_sys and rises with it. It is
     * generated outside this module because a divider flop here would
     * put its edge after the clk_sys edge, and the soft CPU would then
     * sample signals that had already changed on that clk_sys edge. */
    input logic clk_rv,
    /* XRAM's render port runs at twice clk_sys; xram.sv says how, and
     * why clk_ph rides with it. */
    input logic clk_a2,
    input logic clk_ph,
    input logic rst_n,

    output logic [7:0] wiring_tx_data,
    output logic wiring_tx_valid,
    input logic rx_valid,
    input logic [7:0] rx_data,
    output logic wiring_rx_taken,

    output logic [7:0] wiring_rv_tx_data,
    output logic wiring_rv_tx_valid,
    output logic wiring_rv_halted,
    output logic [31:0] wiring_rv_exit_code,

    /* stage_half has to hold the halfword at wiring_stage_addr in any
     * cycle where wiring_stage_pend is high and stage_stall is low, so a
     * slow platform holds stage_stall high until it is there. It also
     * has to stay on stage_half after wiring_stage_pend drops, because
     * the soft CPU reads its byte one clk_rv cycle after its request
     * stops pending. */
    output logic [27:0] wiring_stage_addr,
    output logic wiring_stage_pend,
    input logic stage_stall,
    input logic [15:0] stage_half,

    input logic mach_running,
    output logic wiring_sst_stop_req,
    input logic sst_tcm_sel,
    input logic [14:0] sst_tcm_addr,
    input logic sst_tcm_we,
    input logic [31:0] sst_tcm_wdata,
    output logic [31:0] wiring_sst_tcm_rdata,

    /* Once a save has stopped the machine, it stays stopped until
     * sst_save drops, so the blob, which is the savestate that the host
     * reads a word at a time through sst_rd_idx, does not change while
     * it is read. */
    input logic sst_save,
    input logic sst_load,
    output logic wiring_sst_load_done,
    output logic wiring_sst_load_err,
    output logic wiring_sst_ready,
    input logic [17:0] sst_rd_idx,
    input logic sst_rd_t,
    output logic [31:0] wiring_sst_rdata,
    output logic wiring_sst_rvalid,
    input logic sst_dbg_halt,
    input logic sst_dbg_halt_on_reset,
    input logic sst_dbg_resume,
    output logic wiring_sst_dbg_halted,
    input logic [31:0] sst_dbg_data0,
    output logic [31:0] wiring_sst_dbg_data0,
    output logic wiring_sst_dbg_data0_wen,
    input logic [31:0] sst_dbg_instr,
    input logic sst_dbg_instr_vld,
    output logic wiring_sst_dbg_instr_rdy,
    output logic wiring_sst_dbg_ebreak,
    output logic wiring_sst_dbg_fault,

    /* wiring_host_stb is a one-clock strobe, and host_rdata must be
     * valid from the clock after it until the soft CPU reads it, which
     * is one clk_rv cycle after the soft CPU's request stops pending. */
    output logic [27:0] wiring_host_addr,
    output logic wiring_host_stb,
    output logic wiring_host_we,
    output logic [31:0] wiring_host_wdata,
    input logic [31:0] host_rdata,

    input logic slot_set,
    input logic [31:0] slot_len,
    input logic [7:0] upd_n,
    input logic key_set,
    input logic [8:0] key_code,
    input logic [3:0][31:0] cont_key,
    input logic [3:0][31:0] cont_joy,
    input logic [3:0][15:0] cont_trig,
    output logic wiring_key_pending,

    output logic [15:0] wiring_vid_pixel,
    output logic wiring_vid_de,
    output logic [2:0] wiring_vid_canvas,

    output logic signed [15:0] wiring_aud_l,
    output logic signed [15:0] wiring_aud_r,
    output logic wiring_aud_valid,

    output logic [SCANLINE_W-1:0] wiring_scanline,
    output logic wiring_vid_frame,

    /* Port A has to return its byte before the next PHI2 enable, which
     * is when the 6502 reads it. */
    output logic [15:0] wiring_ram_a_addr,
    output logic [7:0] wiring_ram_a_wdata,
    output logic wiring_ram_a_we,
    input logic [7:0] ram_a_rdata,
    output logic [15:0] wiring_ram_b_addr,
    output logic [7:0] wiring_ram_b_wdata,
    output logic wiring_ram_b_we,
    output logic wiring_ram_b_stb,
    output logic wiring_ram_refill,
    input logic [7:0] ram_b_rdata,
    input logic ram_b_stall,
    input logic ram_hold,
    output logic wiring_phi2_en,
    output logic wiring_cpu_run
);

    localparam int RV_KHZ = SYS_KHZ / 2;

    logic [15:0] phi2_khz;
    logic phi2_raw_en, phi2_en;

    /* When the 6502's RAM is off-chip, both ports share one SRAM chip,
     * so ram_hold suppresses a PHI2 enable while the chip is serving an
     * access or a port B access is waiting. */
    always_comb phi2_en = phi2_raw_en && !ram_hold;
    always_comb wiring_phi2_en = phi2_en;
    phi2 #(.SYS_KHZ(SYS_KHZ)) phi2 (
        .clk(clk_mach),
        .phi2_khz(phi2_khz),
        .phi2_en(phi2_raw_en)
    );

    logic [15:0] cpu_addr, cpu_next_addr;
    logic [7:0] cpu_dout, cpu_din, cpu_next_data;
    logic cpu_we, cpu_next_we;
    logic via_irq;

    logic resb /*verilator public_flat_rw*/;
    /* eng_hold_res rises during a restore while clk_mach is still
     * stopped. It drives the asynchronous resets of the 6502 and the VIA
     * through resb_eff, so both are in reset before clk_mach returns. */
    logic resb_eff;
    always_comb resb_eff = resb && !eng_hold_res;
    always_comb wiring_cpu_run = resb_eff;
    logic cpu_stp;
    logic eng_st_jam, eng_mtime_jam;
    logic [63:0] mtime;
    logic [31:0] eng_jam_mach[4], eng_jam_cpu[5], eng_jam_via[7];
    logic [31:0] eng_jam_ria[12];

    localparam logic [1:0] SEL_MACH = 2'd0;
    localparam logic [1:0] SEL_W65C02 = 2'd1;

    logic [31:0] cpu_st_rdata, via_st_rdata, mach_st_rdata, st_rdata;
    always_comb begin
        /* mtime is saved because the firmware keeps its deadlines as
         * absolute mtime readings in the soft CPU's memory, which is in
         * the blob. If mtime were not restored, each deadline would be
         * off by the difference between the counter's value at the save
         * and its value at the restore. The savestate engine reads mtime
         * while the soft CPU is halted, which is the only time the
         * counter stands still. */
        case (eng_st_idx)
            3'd0: mach_st_rdata = {31'd0, resb};
            3'd1: mach_st_rdata = {16'd0, phi2_khz};
            3'd2: mach_st_rdata = mtime[31:0];
            3'd3: mach_st_rdata = mtime[63:32];
            default: mach_st_rdata = '0;
        endcase
        case (eng_st_sel)
            SEL_MACH:   st_rdata = mach_st_rdata;
            SEL_W65C02: st_rdata = cpu_st_rdata;
            default:    st_rdata = via_st_rdata;
        endcase
    end

    cpu cpu (
        .clk(clk_mach),
        .rst_n(resb_eff),
        .en(phi2_en),
        .data_i(cpu_din),
        .irq_i(via_irq || ria_irq),
        .nmi_i(1'b0),
        .rdy_i(1'b0),
        .res_i(1'b0),
        .cpu_addr(cpu_addr),
        .cpu_data(cpu_dout),
        .cpu_we(cpu_we),
        .cpu_stp(cpu_stp),
        .st_idx(eng_st_idx),
        .cpu_st_rdata(cpu_st_rdata),
        .st_jam(eng_st_jam),
        .st_jam_data(eng_jam_cpu),
        .cpu_next_addr(cpu_next_addr),
        .cpu_next_data(cpu_next_data),
        .cpu_next_we(cpu_next_we)
    );

    logic eng_freeze;
    always_comb wiring_sst_stop_req = eng_freeze;
    always_comb wiring_ram_refill = eng_st_jam;

    logic eng_own;

    logic eng_arr_own, eng_hold_res;
    logic [13:0] eng_mem_addr;
    logic [31:0] eng_mem_wdata;
    logic [1:0] eng_xprog_word;
    logic eng_xram_we, eng_cell_we, eng_xprog_we;
    logic eng_sram_sel, eng_sram_we, eng_regs_we;
    logic eng_stage_pend;
    logic [27:0] eng_stage_addr;
    logic [7:0] eng_regs_word;
    logic [31:0] eng_regs_wdata;
    logic [15:0] eng_sram_addr;
    logic [7:0] eng_sram_wdata;
    logic [31:0] eng_cell_rdata, eng_xprog_rdata;

    logic eng_tcm_sel, eng_dbg_halt, eng_dbg_vld, eng_tcm_we;
    logic [1:0] eng_st_sel;
    logic [31:0] eng_tcm_wdata;
    logic [14:0] eng_tcm_addr;
    logic [2:0] eng_st_idx;
    logic [31:0] eng_dbg_instr, eng_dbg_data0;
    logic eng_dbg_resume;

    sst_engine #(.TCM_WORDS(TCM_WORDS)) engine (
        .clk_sys(clk_sys),
        .rst_n(rst_n),
        .sst_save(sst_save),
        .sst_load(sst_load),
        .sst_engine_load_done(wiring_sst_load_done),
        .sst_engine_load_err(wiring_sst_load_err),
        .sst_engine_busy(eng_own),
        .sst_engine_freeze(eng_freeze),
        .running(mach_running),
        .sst_engine_dbg_halt(eng_dbg_halt),
        .dbg_halted(wiring_sst_dbg_halted),
        .sst_engine_ready(wiring_sst_ready),
        .rd_idx(sst_rd_idx),
        .rd_t(sst_rd_t),
        .sst_engine_rdata(wiring_sst_rdata),
        .sst_engine_rvalid(wiring_sst_rvalid),
        .sst_engine_stage_pend(eng_stage_pend),
        .sst_engine_stage_addr(eng_stage_addr),
        .stage_stall(stage_stall),
        .stage_rdata(stage_rdata),
        .sst_engine_arr_own(eng_arr_own),
        .sst_engine_hold_res(eng_hold_res),
        .sst_engine_mem_addr(eng_mem_addr),
        .sst_engine_mem_wdata(eng_mem_wdata),
        .sst_engine_xram_we(eng_xram_we),
        .sst_engine_regs_word(eng_regs_word),
        .sst_engine_regs_we(eng_regs_we),
        .sst_engine_regs_wdata(eng_regs_wdata),
        .regs_rdata(regs_b_rdata),
        .sst_engine_sram_sel(eng_sram_sel),
        .sst_engine_sram_addr(eng_sram_addr),
        .sst_engine_sram_we(eng_sram_we),
        .sst_engine_sram_wdata(eng_sram_wdata),
        .sram_rdata(sram_b_rdata),
        .sram_stall(ram_b_stall),
        .sst_engine_cell_we(eng_cell_we),
        .sst_engine_xprog_we(eng_xprog_we),
        .sst_engine_xprog_word(eng_xprog_word),
        .xram_rdata(xram_sst_rdata),
        .cell_rdata(eng_cell_rdata),
        .xprog_rdata(eng_xprog_rdata),
        .sst_engine_tcm_sel(eng_tcm_sel),
        .sst_engine_tcm_addr(eng_tcm_addr),
        .sst_engine_tcm_we(eng_tcm_we),
        .sst_engine_tcm_wdata(eng_tcm_wdata),
        .tcm_rdata(wiring_sst_tcm_rdata),
        .sst_engine_st_sel(eng_st_sel),
        .sst_engine_st_idx(eng_st_idx),
        .st_rdata(st_rdata),
        .sst_engine_st_jam(eng_st_jam),
        .sst_engine_mtime_jam(eng_mtime_jam),
        .sst_engine_jam_mach(eng_jam_mach),
        .sst_engine_jam_cpu(eng_jam_cpu),
        .sst_engine_jam_via(eng_jam_via),
        .sst_engine_jam_ria(eng_jam_ria),
        .sst_engine_dbg_data0(eng_dbg_data0),
        .sst_engine_dbg_resume(eng_dbg_resume),
        .sst_engine_dbg_instr(eng_dbg_instr),
        .sst_engine_dbg_instr_vld(eng_dbg_vld),
        .dbg_instr_rdy(wiring_sst_dbg_instr_rdy),
        .dbg_ebreak(wiring_sst_dbg_ebreak),
        .dbg_data0(wiring_sst_dbg_data0),
        .dbg_data0_wen(wiring_sst_dbg_data0_wen)
    );

    logic sel_via, sel_ria;

    /* Every 6502 write also lands in RAM, including a write to
     * $FF00-$FFFF, where 6502 reads come from the VIA, the RIA or open
     * bus instead. */
    logic [7:0] sram_rdata;
    logic [7:0] sram_b_rdata;
    generate
        if (EXT_RAM) begin : g_ram_ext
            always_comb begin
                /* On the jam, port A is given the restored cpu_addr, so
                 * the refill fetches the byte the restored 6502 reads on
                 * its first PHI2 enable rather than one fetched before
                 * the restore.
                 *
                 * The address comes from the jam word and not from
                 * cpu_next_addr, because the cpu flops have not taken
                 * the jam yet and still hold the reset values that
                 * eng_hold_res forced. */
                wiring_ram_a_addr = eng_st_jam ? eng_jam_cpu[4][31:16]
                    : cpu_next_addr;
                wiring_ram_a_wdata = cpu_next_data;
                wiring_ram_a_we = cpu_next_we && !eng_st_jam;
                wiring_ram_b_addr = eng_sram_sel
                    ? eng_sram_addr : soc_addr[15:0];
                wiring_ram_b_wdata = eng_sram_sel
                    ? eng_sram_wdata : soc_wbyte;
                wiring_ram_b_we = eng_sram_sel ? eng_sram_we
                    : (soc_we && !eng_arr_own);
                wiring_ram_b_stb = eng_sram_sel
                    || (soc_pend && soc_sel_sram && !eng_arr_own);
                sram_rdata = ram_a_rdata;
                sram_b_rdata = ram_b_rdata;
            end
        end else begin : g_ram_bram
            sram sram (
                .clk(clk_sys),
                .sst_own(eng_arr_own),
                .sst_addr(eng_sram_addr),
                .sst_we(eng_sram_we),
                .sst_wdata(eng_sram_wdata),
                .a_addr(cpu_addr),
                .a_wdata(cpu_dout),
                .a_we(cpu_we && phi2_en),
                .sram_a_rdata(sram_rdata),
                .b_addr(soc_addr[15:0]),
                .b_wdata(soc_wbyte),
                .b_we(soc_stb && soc_we && soc_sel_sram),
                .sram_b_rdata(sram_b_rdata)
            );
            always_comb begin
                wiring_ram_a_addr = '0;
                wiring_ram_a_wdata = '0;
                wiring_ram_a_we = 1'b0;
                wiring_ram_b_addr = '0;
                wiring_ram_b_wdata = '0;
                wiring_ram_b_we = 1'b0;
                wiring_ram_b_stb = 1'b0;
            end
            /* verilator lint_off UNUSEDSIGNAL */
            logic unused_ram;
            always_comb unused_ram = ^{ram_a_rdata, ram_b_rdata,
                                       cpu_next_addr, cpu_next_data,
                                       cpu_next_we, eng_sram_sel};
            /* verilator lint_on UNUSEDSIGNAL */
        end
    endgenerate

    logic ria_irq;
    logic [7:0] via_data;
    via via (
        .clk(clk_mach),
        .rst_n(resb_eff),
        .en(phi2_en),
        .cs(sel_via),
        .we(cpu_we),
        .rs(cpu_addr[3:0]),
        .data_i(cpu_dout),
        .via_data(via_data),
        .via_irq(via_irq),
        .st_idx(eng_st_idx),
        .via_st_rdata(via_st_rdata),
        .st_jam(eng_st_jam),
        .st_jam_data(eng_jam_via)
    );

    /* Everything the soft CPU drives into the machine is taken on the
     * falling edge of clk_sys first. clk_rv and clk_sys rise together, so
     * the soft CPU's outputs change just after a rising edge, and a
     * register taking one on that same edge would race it: the outcome
     * would depend on the skew between the two clock trees, change from
     * one fit to the next, and never show in simulation. By the falling
     * edge they have been stable for half a period, and that half period
     * is the relationship the analyzer checks. The one thing formed from
     * the raw outputs is the ready the soft CPU reads back, because that
     * path ends in the soft CPU's own clock and has the whole of it.
     *
     * The console valid, which can stay high for more than one clk_sys
     * cycle, is narrowed to one on the way, and slot_set and key_set are
     * stretched by one clk_mach cycle so that a clk_rv edge samples
     * them. */
    logic rv_tx_valid_raw, rv_tx_valid_n, rv_tx_valid_q;
    logic [7:0] rv_tx_data;
    logic slot_set_q, key_set_q;
    logic soc_stb, soc_we, soc_pend;
    logic rv_bus_pend, rv_bus_we;
    logic [31:0] rv_bus_addr, rv_bus_wdata;
    logic [3:0] rv_bus_wstrb;
    logic [15:0] rv_phi2_khz;
    logic [63:0] rv_mtime;
    logic [31:0] rv_tcm_rdata, rv_dbg_data0, rv_exit_code;
    logic rv_dbg_halted, rv_dbg_data0_wen, rv_dbg_instr_rdy, rv_dbg_ebreak,
        rv_dbg_fault, rv_halted, rv_key_pending;
    initial begin
        soc_pend = 1'b0;
        soc_we = 1'b0;
        soc_addr = '0;
        soc_wdata = '0;
        soc_wstrb = '0;
        rv_tx_valid_n = 1'b0;
        wiring_rv_tx_data = '0;
        phi2_khz = 16'd8000;
        mtime = '0;
        wiring_sst_dbg_halted = 1'b0;
        wiring_sst_dbg_data0 = '0;
        wiring_sst_dbg_data0_wen = 1'b0;
        wiring_sst_dbg_instr_rdy = 1'b0;
        wiring_sst_dbg_ebreak = 1'b0;
        wiring_sst_dbg_fault = 1'b0;
        wiring_sst_tcm_rdata = '0;
        wiring_rv_halted = 1'b0;
        wiring_rv_exit_code = '0;
        wiring_key_pending = 1'b0;
    end
    always_ff @(negedge clk_sys) begin
        soc_pend <= rv_bus_pend;
        soc_we <= rv_bus_we;
        soc_addr <= rv_bus_addr;
        soc_wdata <= rv_bus_wdata;
        soc_wstrb <= rv_bus_wstrb;
        rv_tx_valid_n <= rv_tx_valid_raw;
        wiring_rv_tx_data <= rv_tx_data;
        phi2_khz <= rv_phi2_khz;
        mtime <= rv_mtime;
        wiring_sst_dbg_halted <= rv_dbg_halted;
        wiring_sst_dbg_data0 <= rv_dbg_data0;
        wiring_sst_dbg_data0_wen <= rv_dbg_data0_wen;
        wiring_sst_dbg_instr_rdy <= rv_dbg_instr_rdy;
        wiring_sst_dbg_ebreak <= rv_dbg_ebreak;
        wiring_sst_dbg_fault <= rv_dbg_fault;
        wiring_sst_tcm_rdata <= rv_tcm_rdata;
        wiring_rv_halted <= rv_halted;
        wiring_rv_exit_code <= rv_exit_code;
        wiring_key_pending <= rv_key_pending;
    end
    initial begin
        rv_tx_valid_q = 1'b0;
        slot_set_q = 1'b0;
        key_set_q = 1'b0;
    end
    always_ff @(posedge clk_mach) begin
        rv_tx_valid_q <= rv_tx_valid_n;
        slot_set_q <= slot_set;
        key_set_q <= key_set;
    end
    always_comb wiring_rv_tx_valid = rv_tx_valid_n && !rv_tx_valid_q;

    /* An access is taken on the first clk_mach edge that finds it pending
     * and its target able to finish it: XRAM once its port is free, the
     * SRAM and the staging store once they hold the byte, everything
     * else at once. That decision is the machine's, made from the sampled
     * request on its own edge, so it can never run ahead of the stalls
     * it depends on. soc_stb is the request while it waits, one clock for
     * the targets that never stall. soc_taken then stays high until
     * soc_pend drops, because the soft CPU samples it only on clk_rv
     * edges, which are every second clk_sys edge. */
    logic soc_taken, soc_take;
    initial soc_taken = 1'b0;
    always_comb soc_stb = soc_pend && !soc_taken;
    always_comb soc_take = soc_stb
        && (soc_sel_xram ? xram_go
            : soc_sel_sram ? !ram_b_stall
            : soc_sel_stage ? !stage_stall : 1'b1);
    always_ff @(posedge clk_mach) begin
        if (!soc_pend)
            soc_taken <= 1'b0;
        else if (soc_take)
            soc_taken <= 1'b1;
    end
    logic [31:0] soc_addr, soc_wdata;
    logic [3:0] soc_wstrb;
    logic [31:0] soc_rdata;

    /* mtime_acc is clocked by clk_rv, so the rate here is RV_KHZ and not
     * SYS_KHZ. At the default 50.4 MHz clk_sys, a microsecond is 25.2
     * clk_rv cycles, so adding 10 per cycle and wrapping at RV_KHZ / 100,
     * which is 252, counts microseconds exactly. */
    soc #(
        .MTIME_ADD(10),
        .MTIME_WRAP(RV_KHZ / 100),
        .TCM_INIT_FILE(TCM_INIT_FILE)
    ) soc (
        .clk(clk_rv),
        .rst_n(rst_n),
        .soc_phi2_khz(rv_phi2_khz),
        .sst_dbg_halt(sst_dbg_halt || eng_dbg_halt),
        .sst_dbg_halt_on_reset(sst_dbg_halt_on_reset),
        .sst_dbg_resume(sst_dbg_resume || eng_dbg_resume),
        .soc_dbg_halted(rv_dbg_halted),
        .sst_dbg_data0(eng_own ? eng_dbg_data0 : sst_dbg_data0),
        .soc_dbg_data0(rv_dbg_data0),
        .soc_dbg_data0_wen(rv_dbg_data0_wen),
        .sst_dbg_instr(eng_own ? eng_dbg_instr : sst_dbg_instr),
        .sst_dbg_instr_vld(eng_own ? eng_dbg_vld : sst_dbg_instr_vld),
        .soc_dbg_instr_rdy(rv_dbg_instr_rdy),
        .soc_dbg_ebreak(rv_dbg_ebreak),
        .soc_dbg_fault(rv_dbg_fault),
        .sst_phi2_we(eng_st_jam),
        .sst_phi2_wdata(eng_jam_mach[1][15:0]),
        .soc_mtime(rv_mtime),
        .sst_mtime_we(eng_mtime_jam),
        .sst_mtime_wdata({eng_jam_mach[3], eng_jam_mach[2]}),
        .sst_tcm_sel(eng_own ? eng_tcm_sel : sst_tcm_sel),
        .sst_tcm_addr(eng_own ? eng_tcm_addr : sst_tcm_addr),
        .sst_tcm_we(eng_own ? eng_tcm_we : sst_tcm_we),
        .sst_tcm_wdata(eng_own ? eng_tcm_wdata : sst_tcm_wdata),
        .soc_tcm_rdata(rv_tcm_rdata),
        .soc_tx_data(rv_tx_data),
        .soc_tx_valid(rv_tx_valid_raw),
        .soc_halted(rv_halted),
        .soc_exit_code(rv_exit_code),
        .slot_set(slot_set || slot_set_q),
        .slot_len(slot_len),
        .upd_n(upd_n),
        .key_set(key_set || key_set_q),
        .cont_key(cont_key),
        .cont_joy(cont_joy),
        .cont_trig(cont_trig),
        .key_code(key_code),
        .soc_key_pending(rv_key_pending),
        .bus_taken(soc_taken),
        .soc_bus_pend(rv_bus_pend),
        .soc_bus_we(rv_bus_we),
        .soc_bus_addr(rv_bus_addr),
        .soc_bus_wdata(rv_bus_wdata),
        .soc_bus_wstrb(rv_bus_wstrb),
        .bus_rdata(soc_rdata)
    );

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_bus;
    always_comb unused_bus = ^{soc_addr[27:16]};
    /* verilator lint_on UNUSEDSIGNAL */
    logic [7:0] soc_wbyte;
    always_comb begin
        soc_wbyte = soc_wdata[7:0];
        if (soc_wstrb[1])
            soc_wbyte = soc_wdata[15:8];
        if (soc_wstrb[2])
            soc_wbyte = soc_wdata[23:16];
        if (soc_wstrb[3])
            soc_wbyte = soc_wdata[31:24];
    end

    logic soc_sel_sram, soc_sel_regs, soc_sel_ctl, soc_sel_stage,
        soc_sel_vid, soc_sel_xram, soc_sel_aud, soc_sel_host;
    always_comb begin
        soc_sel_sram = soc_addr[31:28] == 4'h1;
        soc_sel_xram = soc_addr[31:28] == 4'h3;
        soc_sel_regs = soc_addr[31:28] == 4'h2;
        soc_sel_ctl = soc_addr[31:28] == 4'h4;
        soc_sel_stage = soc_addr[31:28] == 4'h6;
        soc_sel_vid = soc_addr[31:28] == 4'h5;
        soc_sel_aud = soc_addr[31:28] == 4'h7;
        soc_sel_host = soc_addr[31:28] == 4'h8;
    end

    always_comb begin
        wiring_host_addr = soc_addr[27:0];
        wiring_host_stb = soc_stb && soc_sel_host;
        wiring_host_we = soc_we;
        wiring_host_wdata = soc_wdata;
    end

    /* wiring_stage_addr follows soc_addr while the soft CPU's request is
     * pending, which starts before its strobe, so a slow platform can
     * fetch the byte before the strobe. After soc_pend drops,
     * stage_addr_q, captured at the strobe, holds the address until the
     * soft CPU has read the byte. */
    logic [27:0] stage_addr_q;
    always_comb begin
        /* During a load the engine reads the staging store directly
         * while clk_mach is stopped, which works because the store does
         * not run on clk_mach. */
        wiring_stage_pend = eng_stage_pend || (soc_pend && soc_sel_stage);
        wiring_stage_addr = eng_stage_pend ? eng_stage_addr
            : ((soc_pend && soc_sel_stage) ? soc_addr[27:0] : stage_addr_q);
    end
    /* The byte is picked out of the halfword by a registered address, the
     * engine's own or the one the soft CPU's strobe captured, never the
     * live one: that is the copy taken on the falling edge, and a path
     * from it into the soft CPU's load data would have half a period
     * where this one has the whole. The strobe lands a clock before the
     * soft CPU can read, so stage_addr_q is right by then. */
    logic [7:0] stage_rdata;
    always_comb stage_rdata = (eng_stage_pend ? eng_stage_addr[0]
                                              : stage_addr_q[0])
        ? stage_half[15:8] : stage_half[7:0];

    logic api_pending;
    logic soc_ctl_api;
    logic [31:0] regs_b_rdata, regs_b_q;
    logic [31:0] vid_b_rdata;
    logic [2:0] soc_rsel;
    initial begin
        soc_rsel = 3'd0;
        soc_ctl_api = 1'b0;
        stage_addr_q = '0;
    end
    always_ff @(posedge clk_mach) begin
        if (soc_stb) begin
            soc_rsel <= soc_sel_regs ? 3'd1
                : (soc_sel_ctl ? 3'd2
                : (soc_sel_stage ? 3'd3
                : (soc_sel_vid ? 3'd4
                : (soc_sel_xram ? 3'd5
                : (soc_sel_host ? 3'd6 : 3'd0)))));
            soc_ctl_api <= soc_addr[2];
            stage_addr_q <= soc_addr[27:0];
            /* regs_b_q is captured at the strobe, because a read of word
             * 16 of the regs window pops the console queue on this edge
             * and regs_b_rdata then shows the next byte. */
            regs_b_q <= regs_b_rdata;
        end
    end

    initial resb = 1'b0;
    always_ff @(posedge clk_mach)
        if (eng_st_jam) resb <= eng_jam_mach[0][0];
        else if (soc_stb && soc_we && soc_sel_ctl && !soc_addr[2])
            resb <= soc_wbyte[0];

    /* The byte-wide windows put their byte on all four lanes, because a
     * Hazard3 byte load picks its lane from the low address bits. */
    logic [7:0] soc_rbyte;
    always_comb begin
        case (soc_rsel)
            3'd2: soc_rbyte = soc_ctl_api ? {7'b0, api_pending}
                : {6'b0, cpu_stp, resb_eff};
            3'd3: soc_rbyte = stage_rdata;
            3'd5: soc_rbyte = xram_b_hold;
            default: soc_rbyte = sram_b_rdata;
        endcase
        soc_rdata = soc_rsel == 3'd1 ? regs_b_q
            : (soc_rsel == 3'd4 ? vid_b_rdata
               : (soc_rsel == 3'd6 ? host_rdata : {4{soc_rbyte}}));
    end

    logic [7:0] ria_data;
    regs ria (
        .clk(clk_mach),
        .clk_mem(clk_sys),
        .en(phi2_en),
        .cs(sel_ria),
        .we(cpu_we),
        .rs(cpu_addr[4:0]),
        .data_i(cpu_dout),
        .regs_data(ria_data),
        .regs_tx_data(wiring_tx_data),
        .regs_tx_valid(wiring_tx_valid),
        .rx_valid(rx_valid),
        .rx_data(rx_data),
        .regs_rx_taken(wiring_rx_taken),
        .vsync_pulse(prog_vsync_pulse),
        .regs_irq(ria_irq),
        .regs_xr_busy(xr_busy),
        .regs_xr_we(xr_we),
        .regs_xr_addr(xr_addr),
        .regs_xr_wdata(xr_wdata),
        .xr_rdata(xram_b_rdata),
        .xr_cpu_want(soc_pend && soc_sel_xram),
        .sst_jam(eng_st_jam),
        .sst_jam_data(eng_jam_ria),
        .sst_own(eng_arr_own),
        .sst_word(eng_regs_word),
        .sst_we(eng_regs_we),
        .sst_wdata(eng_regs_wdata),
        .b_we(soc_stb && soc_we && soc_sel_regs),
        .b_re(soc_stb && !soc_we && soc_sel_regs),
        .b_word(soc_addr[9:2]),
        .b_wstrb(soc_wstrb),
        .b_wdata(soc_wdata),
        .regs_b_rdata(regs_b_rdata),
        .regs_api_pending(api_pending),
        .api_ack(soc_stb && soc_we && soc_sel_ctl && soc_addr[2])
    );

    bus bus (
        .clk(clk_mach),
        .en(phi2_en),
        .addr(cpu_addr),
        .we(cpu_we),
        .dout(cpu_dout),
        .ria_data(ria_data),
        .via_data(via_data),
        .sram_rdata(sram_rdata),
        .bus_din(cpu_din),
        .bus_sel_via(sel_via),
        .bus_sel_ria(sel_ria)
    );

    logic [9:0] vid_h /*verilator public_flat_rd*/;
    logic [9:0] vid_v /*verilator public_flat_rd*/;
    logic vid_de_full;
    logic vid_de /*verilator public_flat_rd*/;
    logic vid_hsync /*verilator public_flat_rd*/;
    logic vid_vsync /*verilator public_flat_rd*/;
    logic vid_line_start, vid_frame_start;
    always_comb wiring_vid_frame = vid_frame_start;
    logic vid_vsync_pulse;
    logic prog_vsync_pulse;
    logic vid_px_first, vid_px_last;
    timing timing (
        .clk(clk_mach),
        .timing_h(vid_h),
        .timing_v(vid_v),
        .timing_px_first(vid_px_first),
        .timing_px_last(vid_px_last),
        .timing_de(vid_de_full),
        .timing_hsync(vid_hsync),
        .timing_vsync(vid_vsync),
        .timing_line_start(vid_line_start),
        .timing_frame_start(vid_frame_start),
        .timing_vsync_pulse(vid_vsync_pulse)
    );
    always_comb wiring_scanline = vid_v;

    /* The RW engine in regs.sv, which serves the 6502's RW0 and RW1
     * registers, holds XRAM port B while xr_busy is high. Its background
     * refresh yields to a pending soft CPU access, but an access the 6502
     * started does not, so a soft CPU access to XRAM is not taken until
     * xr_busy drops. */
    logic [7:0] xram_b_rdata;
    logic xr_busy, xr_we;
    logic [15:0] xr_addr;
    logic [7:0] xr_wdata;
    logic [31:0] xram_f_rdata, xram_s_rdata, xram_sst_rdata;
    logic [1:0] mf_req;
    logic [13:0] mf_addr[2];
    logic f_rotor, f_sel;
    logic f_any;
    always_comb begin
        f_sel = f_rotor;
        f_any = 1'b0;
        for (int i = 0; i < 2; i++) begin
            logic cand;
            cand = f_rotor ^ 1'(i);
            if (!f_any && mf_req[cand]) begin
                f_sel = cand;
                f_any = 1'b1;
            end
        end
    end
    initial f_rotor = 1'd0;
    always_ff @(posedge clk_mach)
        if (f_any)
            f_rotor <= f_sel + 1'd1;

    logic [7:0] font_bits;
    font font (
        .clk(clk_mach),
        .addr(mf_addr[f_sel]),
        .font_bits(font_bits),
        .w_stb(soc_stb && soc_we && soc_sel_vid && soc_addr[18]),
        .w_addr(soc_addr[13:0]),
        .w_data(soc_wdata)
    );

    /* The fill and the sprite stage each own a slot on XRAM's render
     * port, a word every clock, so a request is always taken: the port
     * reads whatever address each presents, and the word is on that
     * reader's data port two clocks after. */
    logic [1:0] ma_req /*verilator public_flat_rd*/;
    logic [13:0] ma_addr[2];

    logic xram_owed;
    initial xram_owed = 1'b0;
    always_ff @(posedge clk_mach) begin
        if (!soc_pend)
            xram_owed <= 1'b0;
        else if (soc_stb && soc_sel_xram && xr_busy)
            xram_owed <= 1'b1;
        else if (xram_owed && !xr_busy)
            xram_owed <= 1'b0;
    end
    logic xram_go;
    always_comb xram_go = !xr_busy
        && ((soc_stb && soc_sel_xram) || xram_owed);

    /* The PSG and the OPL snoop the RW engine's and the soft CPU's XRAM
     * writes through a registered copy of these signals. xw_host marks a
     * write from the RW engine, which only the 6502 drives, because the
     * PSG starts or releases a voice from its gate bit only on a 6502
     * write, unless the firmware has set gate_any for a restore. */
    logic xw_we, xw_host;
    logic [15:0] xw_addr;
    logic [7:0] xw_wdata;
    always_comb begin
        xw_host = xr_busy;
        xw_we = xr_busy ? (xr_we && !eng_arr_own && !eng_hold_res)
                        : (xram_go && soc_we && !eng_arr_own);
        xw_addr = xr_busy ? xr_addr : soc_addr[15:0];
        xw_wdata = xr_busy ? xr_wdata : soc_wbyte;
    end

    /* XRAM port B registers its read, so its byte arrives one clock
     * after the address, and the RW engine can take port B on that
     * clock. The byte for a soft CPU read is therefore captured one
     * clock after xram_go. */
    logic xram_cap;
    logic [7:0] xram_b_hold;
    initial begin
        xram_cap = 1'b0;
        xram_b_hold = '0;
    end
    always_ff @(posedge clk_mach) begin
        xram_cap <= xram_go && !soc_we;
        if (xram_cap)
            xram_b_hold <= xram_b_rdata;
    end
    /* The snooped write is registered because the soft CPU's side of the
     * xw mux is combinational from its bus and the PSG forwards a
     * snooped byte combinationally into its voice arithmetic, so without
     * the register one path would run from the soft CPU's bus through
     * that arithmetic in a single clk_sys period. Neither the PSG nor
     * the OPL reads XRAM, so a one-clock delay on the snooped write
     * cannot put either out of step with the array. */
    logic qs_we, qs_host;
    logic [15:0] qs_addr;
    logic [7:0] qs_val;
    initial begin
        qs_we = 1'b0;
        qs_host = 1'b0;
        qs_addr = '0;
        qs_val = '0;
    end
    always_ff @(posedge clk_mach) begin
        qs_we <= xw_we;
        qs_host <= xw_host;
        qs_addr <= xw_addr;
        qs_val <= xw_wdata;
    end
    xram xram (
        .clk(clk_sys),
        .clk_mach(clk_mach),
        .clk_a2(clk_a2),
        .clk_ph(clk_ph),
        .f_addr(ma_addr[0]),
        .xram_f_rdata(xram_f_rdata),
        .s_addr(ma_addr[1]),
        .xram_s_rdata(xram_s_rdata),
        .sst_own(eng_arr_own),
        .sst_addr(eng_mem_addr),
        .sst_we(eng_xram_we),
        .sst_wdata(eng_mem_wdata),
        .xram_sst_rdata(xram_sst_rdata),
        .b_addr(xw_addr),
        .b_wdata(xw_wdata),
        .b_we(xw_we),
        .xram_b_rdata(xram_b_rdata)
    );

    logic [2:0] vid_canvas;
    always_comb wiring_vid_canvas = vid_canvas;
    logic [9:0] vid_cw, vid_ch;

    logic [8:0] sched_p_line;
    logic [1:0] sched_p_plane;
    logic [31:0] pm_entry;
    logic [15:0] pm_config;

    prog prog (
        .clk(clk_mach),
        .clk_mem(clk_sys),
        .frame_start(vid_frame_start),
        .v(vid_v),
        .px_first(vid_px_first),
        .prog_vsync_pulse(prog_vsync_pulse),
        .h(vid_h),
        .prog_canvas(vid_canvas),
        .prog_cw(vid_cw),
        .prog_ch(vid_ch),
        .p_line(sched_p_line),
        .p_plane(sched_p_plane),
        .prog_p_entry(pm_entry),
        .prog_p_config(pm_config),
        .s_idx(sp_s_idx),
        .prog_s_data(sp_s_data),
        .sst_own(eng_arr_own),
        .sst_addr(eng_mem_addr[10:0]),
        .sst_word(eng_xprog_word),
        .sst_we(eng_xprog_we),
        .sst_wdata(eng_mem_wdata),
        .prog_sst_rdata(eng_xprog_rdata),
        .b_stb(soc_stb && soc_sel_vid && !soc_addr[18]
               && soc_addr[17]),
        .b_we(soc_we),
        .b_addr(soc_addr[15:0]),
        .b_wdata(soc_wdata)
    );

    /* The beam's derived columns, computed once for the line buffers:
     * the sprite buffers' erase a pixel behind the beam, and the read for
     * the next pixel, which wraps at the line's end. */
    logic vid_h_last;
    logic [9:0] vid_sc_addr, vid_rd_addr;
    always_comb begin
        vid_h_last = vid_h == 10'd799;
        vid_sc_addr = vid_h - 10'd1;
        vid_rd_addr = vid_h_last ? 10'd0 : vid_h + 10'd1;
    end

    /* The row map, derived once in sched.sv and read by every stage. */
    logic [9:0] sched_t;
    logic sched_dbl, sched_pair_start, sched_pair_end, sched_render_now;

    logic [15:0] mode0_pix;
    mode0 mode0 (
        .clk(clk_mach),
        .clk_mem(clk_sys),
        .frame_start(vid_frame_start),
        .h(vid_h),
        .v(vid_v),
        .px_last(vid_px_last),
        .line_start(vid_line_start),
        .cw(vid_cw),
        .t(sched_t),
        .pair_start(sched_pair_start),
        .pair_end(sched_pair_end),
        .mode0_pix(mode0_pix),
        .mode0_f_req(mf_req[1]),
        .mode0_f_addr(mf_addr[1]),
        .f_gnt(f_any && f_sel == 1'd1),
        .f_data(font_bits),
        .sst_own(eng_arr_own),
        .sst_addr(eng_mem_addr),
        .sst_we(eng_cell_we),
        .sst_wdata(eng_mem_wdata),
        .mode0_sst_rdata(eng_cell_rdata),
        .b_stb(soc_stb && soc_sel_vid && !soc_addr[18]
               && !soc_addr[17]),
        .b_we(soc_we),
        .b_addr(soc_addr[16:0]),
        .b_wstrb(soc_wstrb),
        .b_wdata(soc_wdata),
        .mode0_b_rdata(vid_b_rdata)
    );

    logic [15:0] m_pix[3];
    logic [12:0] sp_s_idx;
    logic [31:0] sp_s_data;
    logic [16:0] sp_pix[3];

    logic fl_start;
    logic [2:0] fl_mode;
    logic [15:0] fl_attr, fl_config;
    logic fl_done;
    logic [1:0] fl_px_we;
    logic [9:0] fl_px_addr;
    logic [31:0] fl_px_data;
    logic [1:0] m_px_we[3];
    logic [2:0] m_done;
    logic [2:0] sched_term;
    sched sched (
        .clk(clk_mach),
        .rst_n(rst_n),
        .v(vid_v),
        .h(vid_h),
        .line_start(vid_line_start),
        .cw(vid_cw),
        .ch(vid_ch),
        .sched_t(sched_t),
        .sched_dbl(sched_dbl),
        .sched_pair_start(sched_pair_start),
        .sched_pair_end(sched_pair_end),
        .sched_render_now(sched_render_now),
        .sched_p_line(sched_p_line),
        .sched_p_plane(sched_p_plane),
        .p_entry(pm_entry),
        .p_config(pm_config),
        .sched_e_start(fl_start),
        .sched_e_mode(fl_mode),
        .sched_e_attr(fl_attr),
        .sched_e_config(fl_config),
        .e_done(fl_done),
        .e_px_we(fl_px_we),
        .sched_px_we(m_px_we),
        .sched_done(m_done),
        .sched_term(sched_term)
    );
    fill fill (
        .clk(clk_mach),
        .row_start(vid_row_start),
        .start(fl_start),
        .mode(fl_mode),
        .attr_i(fl_attr),
        .config_ptr_i(fl_config),
        .t_row(sched_p_line),
        .cw(vid_cw),
        .fill_a_req(ma_req[0]),
        .fill_a_addr(ma_addr[0]),
        .a_gnt(ma_req[0]),
        .a_rdata(xram_f_rdata),
        .fill_f_req(mf_req[0]),
        .fill_f_addr(mf_addr[0]),
        .f_gnt(f_any && f_sel == 1'd0),
        .f_data(font_bits),
        .fill_px_we(fl_px_we),
        .fill_px_addr(fl_px_addr),
        .fill_px_data(fl_px_data),
        .fill_done(fl_done)
    );
    genvar gi;
    generate
        for (gi = 0; gi < 3; gi++) begin : gen_mode
            linebuf linebuf (
                .clk(clk_mach),
                .rd_addr(vid_rd_addr),
                .h_last(vid_h_last),
                .px_last(vid_px_last),
                .line_start(vid_line_start),
                .flip_ok(vid_flip_ok),
                .next_ok(vid_next_ok),
                .px_we(m_px_we[gi]),
                .px_addr(fl_px_addr),
                .px_data(fl_px_data),
                .done_i(m_done[gi]),
                .linebuf_pix(m_pix[gi])
            );
        end
    endgenerate

    sprite sprite (
        .clk(clk_mach),
        .sc_addr(vid_sc_addr),
        .rd_addr(vid_rd_addr),
        .h_last(vid_h_last),
        .px_last(vid_px_last),
        .line_start(vid_line_start),
        .cw(vid_cw),
        .t_row(sched_t[8:0]),
        .render_now(sched_render_now),
        .pair_start(sched_pair_start),
        .pair_end(sched_pair_end),
        .sprite_s_idx(sp_s_idx),
        .s_data(sp_s_data),
        .sprite_pix(sp_pix),
        .sprite_a_req(ma_req[1]),
        .sprite_a_addr(ma_addr[1]),
        .a_rdata(xram_s_rdata)
    );

    logic aud_we;
    always_comb aud_we = soc_stb && soc_we && soc_sel_aud;

    logic psg_tick;

    logic signed [15:0] psg_l, psg_r;
    /* verilator lint_off PINCONNECTEMPTY */
    psg psg (
        .clk(clk_mach),
        .xaddr_we(aud_we && soc_addr[5:2] == 4'h0),
        .xaddr_wdata(soc_wdata[15:0]),
        .gate_any_we(aud_we && soc_addr[5:2] == 4'h1),
        .gate_any_wdata(soc_wdata[0]),
        .q_we(qs_we),
        .q_host(qs_host),
        .q_addr(qs_addr),
        .q_val(qs_val),
        .bel_lo_we(aud_we && soc_addr[5:2] == 4'h4),
        .bel_hi_we(aud_we && soc_addr[5:2] == 4'h5),
        .bel_wdata(soc_wdata),
        .psg_l(psg_l),
        .psg_r(psg_r),
        .psg_valid(),
        .psg_tick(psg_tick)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    logic signed [15:0] opl_l;
    logic opl_valid;
    /* verilator lint_off PINCONNECTEMPTY */
    opl opl (
        .clk(clk_mach),
        .xaddr_we(aud_we && soc_addr[5:2] == 4'h2),
        .xaddr_wdata(soc_wdata[15:0]),
        /* The OPL snoops the soft CPU's XRAM writes as well as the RW
         * engine's, because after a restore the firmware rewrites each
         * byte of the OPL's register page to reload the OPL, and those
         * writes come from the soft CPU. */
        .q_we(qs_we),
        .q_addr(qs_addr),
        .q_val(qs_val),
        .opl_out(opl_l),
        .opl_valid(opl_valid),
        .opl_enabled()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    /* The OPL produces a sample every 1014 clocks and the audio output
     * takes one on each psg_tick, every 1050 clocks, so the OPL output
     * is resampled onto the tick. One resampler serves both channels
     * because a YM3812 is mono. */
    logic signed [15:0] opl_rs;
    /* verilator lint_off PINCONNECTEMPTY */
    rsmp rsmp (
        .clk(clk_mach),
        .in_sample(opl_l),
        .in_valid(opl_valid),
        .step(psg_tick),
        .rsmp_out(opl_rs),
        /* rsmp_valid is unused because rsmp updates rsmp_out 52 clocks
         * after a psg_tick that it acts on, and rsmp_out holds that value
         * until the audio output takes it on the next tick. */
        .rsmp_valid()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    logic signed [16:0] eng_l, eng_r;
    always_comb begin
        eng_l = 17'(opl_rs) + 17'(psg_l);
        eng_r = 17'(opl_rs) + 17'(psg_r);
        wiring_aud_l = eng_l < -17'sd32768 ? -16'sd32768
            : eng_l > 17'sd32767 ? 16'sd32767 : 16'(eng_l);
        wiring_aud_r = eng_r < -17'sd32768 ? -16'sd32768
            : eng_r > 17'sd32767 ? 16'sd32767 : 16'(eng_r);
        wiring_aud_valid = psg_tick;
    end

    /* A 320 wide canvas spans two lines of timing per row of graphics, so
     * the row is handed to the scaler on the first line of the pair and the
     * second is blanked. The scaler still receives ch rows; what changed is
     * that the beam now takes the whole frame to cross the canvas. */
    /* A line buffer flips where a pair starts, and next_ok says the line
     * about to start is one; a row of graphics starts where a pair does. */
    logic vid_dbl, vid_flip_ok, vid_next_ok, vid_row_start;
    always_comb vid_dbl = sched_dbl;
    always_comb vid_flip_ok = sched_pair_start;
    always_comb vid_next_ok = sched_pair_end;
    always_comb vid_row_start = vid_line_start && sched_pair_start;
    always_comb vid_de = vid_de_full && vid_h < vid_cw
        && (vid_dbl ? !vid_v[0] && vid_v < {vid_ch[8:0], 1'b0}
                    : vid_v < vid_ch);

    logic [15:0] c_pix[3];
    always_comb
        for (int i = 0; i < 3; i++)
            c_pix[i] = sched_term[i] ? mode0_pix : m_pix[i];
    compose compose (
        .clk(clk_mach),
        .de(vid_de),
        .p0_pix(c_pix[0]),
        .s0_pix(sp_pix[0]),
        .p1_pix(c_pix[1]),
        .s1_pix(sp_pix[1]),
        .p2_pix(c_pix[2]),
        .s2_pix(sp_pix[2]),
        .compose_pix(wiring_vid_pixel),
        .compose_de(wiring_vid_de)
    );

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid;
    always_comb unused_vid = ^{vid_hsync, vid_vsync, vid_vsync_pulse};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
