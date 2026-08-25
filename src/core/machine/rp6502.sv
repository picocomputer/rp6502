/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine, independent of the FPGA platform hosting it.
 *
 * Two quirks the map inherits and nothing else would predict: every write
 * also lands in the SRAM shadow, and $FF00-$FFCF reads float at the last
 * value the bus carried.
 */

module rp6502
    import rp6502_pkg::*;
#(
    /* Empty in simulation, which loads the arrays through the bench. */
    parameter TCM_INIT_FILE = "",
    /* clk_sys in kHz, which the PHI2 accumulator counts against. */
    parameter int SYS_KHZ = 50400,
    /* Where the 6502's 64 KB lives. Zero builds it here out of block
     * memory; one exports the two ports and lets the platform find the
     * storage, which on the Pocket is a real SRAM chip. */
    parameter bit EXT_RAM = 0
) (
    /* Two clocks, and only one of them stops. clk_sys carries the
     * arrays and the savestate serializer, which have to keep working
     * while the machine does not; clk_mach carries the machine -- the
     * 6502, the VIA, the RIA, the video, the audio -- and is taken away
     * at a stroke when a savestate is being made. Nothing in the
     * machine is gated, because there is nothing to gate: it simply has
     * no clock. */
    input logic clk_sys,
    input logic clk_mach,
    /* Half clk_sys, rising with it. Made outside: a divider made here
     * would rise after this module's registers settle on the same edge,
     * and a master clocked that late reads a ready not yet published. */
    input logic clk_rv,
    input logic rst_n,

    /* Console: TX bytes out of $FFE1, RX bytes offered toward $FFE2. */
    output logic [7:0] rp6502_tx_data,
    output logic rp6502_tx_valid,
    input logic rx_valid,
    input logic [7:0] rx_data,
    output logic rp6502_rx_taken,

    output logic [7:0] rp6502_rv_tx_data,
    output logic rp6502_rv_tx_valid,
    output logic rp6502_rv_halted,
    output logic [31:0] rp6502_rv_exit_code,

    /* The address holds from the pending request. A combinational
     * platform answers before the next system clock; a slow one holds
     * stage_stall until its byte stands on stage_rdata. */
    output logic [27:0] rp6502_stage_addr,
    output logic rp6502_stage_pend,
    input logic stage_stall,
    input logic [7:0] stage_rdata,

    /* Whether the machine has its clock. Whoever owns the clock tree
     * answers this; the serializer waits on it both ways. */
    input logic mach_running,
    /* Raised while a savestate wants the machine stopped. Whoever owns
     * the clocks cuts them when this and the boundary are both true,
     * and puts them back when it drops. */
    output logic rp6502_sst_stop_req,
    /* The soft CPU's own memory, which is not on the machine's bus. */
    input logic sst_tcm_sel,
    input logic [14:0] sst_tcm_addr,
    input logic sst_tcm_we,
    input logic [31:0] sst_tcm_wdata,
    output logic [31:0] rp6502_sst_tcm_rdata,

    /* A savestate, asked for and read back a word at a time. Holding
     * sst_save stops the machine and keeps it stopped; the blob stands
     * still underneath the reader until it is let go. */
    input logic sst_save,
    input logic sst_load,
    output logic rp6502_sst_load_done,
    output logic rp6502_sst_load_err,
    output logic rp6502_sst_ready,
    input logic [17:0] sst_rd_idx,
    input logic sst_rd_t,
    output logic [31:0] rp6502_sst_rdata,
    output logic rp6502_sst_rvalid,
    /* The soft CPU's own halt, and the port its registers come out
     * through. It resumes at the instruction it stopped in front of;
     * nothing here restarts it. */
    input logic sst_dbg_halt,
    input logic sst_dbg_halt_on_reset,
    input logic sst_dbg_resume,
    output logic rp6502_sst_dbg_halted,
    input logic [31:0] sst_dbg_data0,
    output logic [31:0] rp6502_sst_dbg_data0,
    output logic rp6502_sst_dbg_data0_wen,
    input logic [31:0] sst_dbg_instr,
    input logic sst_dbg_instr_vld,
    output logic rp6502_sst_dbg_instr_rdy,
    output logic rp6502_sst_dbg_ebreak,
    output logic rp6502_sst_dbg_fault,

    /* A word-wide port onto whatever the board has that the machine does
     * not. One-clock strobe, answer the clock after. */
    output logic [27:0] rp6502_host_addr,
    output logic rp6502_host_stb,
    output logic rp6502_host_we,
    output logic [31:0] rp6502_host_wdata,
    input logic [31:0] host_rdata,

    input logic slot_set,
    input logic [31:0] slot_len,
    /* How many times the host has announced a slot. Zero forever on a
     * platform that never announces one. */
    input logic [7:0] upd_n,
    input logic key_set,
    input logic [8:0] key_code,
    input logic [3:0][31:0] cont_key,
    input logic [3:0][31:0] cont_joy,
    input logic [3:0][15:0] cont_trig,
    output logic rp6502_key_pending,

    /* The canvas names the scaler's mode; de already is the canvas. */
    output logic [15:0] rp6502_vid_pixel,
    output logic rp6502_vid_de,
    output logic [2:0] rp6502_vid_canvas,

    output logic signed [15:0] rp6502_aud_l,
    output logic signed [15:0] rp6502_aud_r,
    output logic rp6502_aud_valid,

    output logic [RP6502_SCANLINE_W-1:0] rp6502_scanline,
    output logic rp6502_vid_frame,

    /* Port A is the machine's own and answers within the PHI2 period;
     * port B is the soft CPU's window and can be told to wait. */
    output logic [15:0] rp6502_ram_a_addr,
    output logic [7:0] rp6502_ram_a_wdata,
    output logic rp6502_ram_a_we,
    input logic [7:0] ram_a_rdata,
    output logic [15:0] rp6502_ram_b_addr,
    output logic [7:0] rp6502_ram_b_wdata,
    output logic rp6502_ram_b_we,
    output logic rp6502_ram_b_stb,
    /* One port-A fetch for the jammed address, at the restore's end. */
    output logic rp6502_ram_refill,
    input logic [7:0] ram_b_rdata,
    input logic ram_b_stall,
    input logic ram_hold,
    /* The gated pulse: a cycle the hold suppressed is a cycle the 6502
     * did not take, and must not be fetched for. */
    output logic rp6502_phi2_en,
    output logic rp6502_cpu_run
);

    /* The soft CPU measures time against its own clock, not this one. */
    localparam int RV_KHZ = SYS_KHZ / 2;

    logic [15:0] phi2_khz;
    logic phi2_raw_en, phi2_en;
    always_comb bus_rdy = !(bus_sel_xram && xr_busy)
        && !(bus_sel_stage && stage_stall)
        && !(bus_sel_sram && ram_b_stall);

    /* The soft CPU's window onto the 6502's RAM shares a port with the
     * 6502 when the RAM is off-chip. Nothing asks for this today; it
     * exists so a firmware that did would be slow rather than
     * deadlocked. */
    always_comb phi2_en = phi2_raw_en && !ram_hold;
    always_comb rp6502_phi2_en = phi2_en;
    phi2_div #(.SYS_KHZ(SYS_KHZ)) phi2_div (
        .clk(clk_mach),
        .phi2_khz(phi2_khz),
        .phi2_div_en(phi2_raw_en)
    );

    logic [15:0] cpu_addr, cpu_next_addr;
    logic [7:0] cpu_dout, cpu_din, cpu_next_data;
    logic cpu_we, cpu_next_we;
    logic w65c22_irq;
    /* Opcode-fetch marker, which is where a freeze is allowed to land. */

    /* RESB inverted, reaching the 6502 and the 6522 and nothing else. A
     * register rather than a gate with the platform's reset, which
     * already clears this flop asynchronously. */
    logic resb /*verilator public_flat_rw*/;
    /* What the 6502 and the VIA actually see. The hold is a restore's:
     * asynchronous, so it lands while the machine has no clock, and the
     * clock then returns over a core already in reset. */
    logic resb_eff;
    always_comb resb_eff = resb && !eng_hold_res;
    always_comb rp6502_cpu_run = resb_eff;
    logic cpu_stp;
    /* Read a word at a time and written all at once. The read needs no
     * clock -- a flop's output is simply there -- and the write needs
     * one edge for the lot, because a restore lands with the machine's
     * clock already back and a jam spread over twelve edges would let
     * it run through eleven of them. */
    logic eng_st_jam, eng_mtime_jam;
    logic [63:0] mtime;
    logic [31:0] eng_jam_mach[4], eng_jam_w65c02[5], eng_jam_w65c22[7];
    logic [31:0] eng_jam_ria[12];

    /* Three sets of flops share one indexed port: the machine's own,
     * the 6502's, and the VIA's. */
    localparam logic [1:0] SEL_MACH = 2'd0;
    localparam logic [1:0] SEL_W65C02 = 2'd1;

    logic [31:0] w65c02_st_rdata, w65c22_st_rdata, mach_st_rdata, st_rdata;
    always_comb begin
        /* The 6502's clock rate is the soft CPU's to set and it sets it
         * once, so a wake that came up at the reset rate would run the
         * machine at a speed nothing was going to correct.
         *
         * The microsecond counter is here for the same reason and a
         * sharper one: the firmware's every deadline is an absolute
         * reading of it, held in memory the blob does carry, so a
         * counter that came back at zero would put all of them the
         * machine's previous uptime into the future. It is read while
         * the core is halted, which is the only time it stands still.
         */
        case (eng_st_idx)
            3'd0: mach_st_rdata = {31'd0, resb};
            3'd1: mach_st_rdata = {16'd0, phi2_khz};
            3'd2: mach_st_rdata = mtime[31:0];
            3'd3: mach_st_rdata = mtime[63:32];
            default: mach_st_rdata = '0;
        endcase
        case (eng_st_sel)
            SEL_MACH:   st_rdata = mach_st_rdata;
            SEL_W65C02: st_rdata = w65c02_st_rdata;
            default:    st_rdata = w65c22_st_rdata;
        endcase
    end

    w65c02 w65c02 (
        .clk(clk_mach),
        .rst_n(resb_eff),
        .en(phi2_en),
        .data_i(cpu_din),
        .irq_i(w65c22_irq || ria_irq),
        .nmi_i(1'b0),
        .rdy_i(1'b0),
        .res_i(1'b0),
        .w65c02_addr(cpu_addr),
        .w65c02_data(cpu_dout),
        .w65c02_we(cpu_we),
        .w65c02_stp(cpu_stp),
        .st_idx(eng_st_idx),
        .w65c02_st_rdata(w65c02_st_rdata),
        .st_jam(eng_st_jam),
        .st_jam_data(eng_jam_w65c02),
        .w65c02_next_addr(cpu_next_addr),
        .w65c02_next_data(cpu_next_data),
        .w65c02_next_we(cpu_next_we)
    );

    logic eng_freeze;
    always_comb rp6502_sst_stop_req = eng_freeze;
    always_comb rp6502_ram_refill = eng_st_jam;

    /* The serializer owns the soft CPU's debug port and its memory for
     * as long as it is doing anything -- which starts before the clock
     * is cut, because the registers come out through that port while
     * the core still has one, and ends after it is back, because they
     * go in the same way. */
    logic eng_own;

    /* The serializer's own way into every array. It does not go through
     * the machine's bus any more: the bus is machine logic and machine
     * logic has no clock while a savestate is being made. The arrays do
     * -- their addresses come from logic that is standing still, so
     * they sit there doing nothing until this side asks. */
    /* The serializer's ownership of every array, held for the whole
     * stopped window rather than per access: machine logic freezes
     * wherever it was, and a write enable frozen high would otherwise
     * re-fire into an array that kept its clock. And the reset hold a
     * restore raises before the clock returns, so the 6502 and the VIA
     * come back into a rebuilt world already in reset. */
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

    sst_engine engine (
        .clk_sys(clk_sys),
        .rst_n(rst_n),
        .sst_save(sst_save),
        .sst_load(sst_load),
        .sst_engine_load_done(rp6502_sst_load_done),
        .sst_engine_load_err(rp6502_sst_load_err),
        .sst_engine_busy(eng_own),
        .sst_engine_freeze(eng_freeze),
        .running(mach_running),
        .sst_engine_dbg_halt(eng_dbg_halt),
        .dbg_halted(rp6502_sst_dbg_halted),
        .sst_engine_ready(rp6502_sst_ready),
        .rd_idx(sst_rd_idx),
        .rd_t(sst_rd_t),
        .sst_engine_rdata(rp6502_sst_rdata),
        .sst_engine_rvalid(rp6502_sst_rvalid),
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
        .xram_rdata(xram_a_rdata),
        .cell_rdata(eng_cell_rdata),
        .xprog_rdata(eng_xprog_rdata),
        .sst_engine_tcm_sel(eng_tcm_sel),
        .sst_engine_tcm_addr(eng_tcm_addr),
        .sst_engine_tcm_we(eng_tcm_we),
        .sst_engine_tcm_wdata(eng_tcm_wdata),
        .tcm_rdata(rp6502_sst_tcm_rdata),
        .sst_engine_st_sel(eng_st_sel),
        .sst_engine_st_idx(eng_st_idx),
        .st_rdata(st_rdata),
        .sst_engine_st_jam(eng_st_jam),
        .sst_engine_mtime_jam(eng_mtime_jam),
        .sst_engine_jam_mach(eng_jam_mach),
        .sst_engine_jam_w65c02(eng_jam_w65c02),
        .sst_engine_jam_w65c22(eng_jam_w65c22),
        .sst_engine_jam_ria(eng_jam_ria),
        .sst_engine_dbg_data0(eng_dbg_data0),
        .sst_engine_dbg_resume(eng_dbg_resume),
        .sst_engine_dbg_instr(eng_dbg_instr),
        .sst_engine_dbg_instr_vld(eng_dbg_vld),
        .dbg_instr_rdy(rp6502_sst_dbg_instr_rdy),
        .dbg_ebreak(rp6502_sst_dbg_ebreak),
        .dbg_data0(rp6502_sst_dbg_data0),
        .dbg_data0_wen(rp6502_sst_dbg_data0_wen)
    );

    logic sel_w65c22, sel_ria, sel_open;
    always_comb begin
        sel_w65c22 = cpu_addr[15:4] == 12'hFFD;
        sel_ria = cpu_addr[15:5] == 11'b1111_1111_111;
        sel_open = cpu_addr[15:8] == 8'hFF && !sel_w65c22 && !sel_ria;
    end

    /* Every write lands in the shadow, whatever else it hits. */
    logic [7:0] sram_rdata;
    logic [7:0] sram_b_rdata;
    generate
        if (EXT_RAM) begin : g_ram_ext
            always_comb begin
                /* On the jam, port A refetches the RESTORED current
                 * address: the read the frozen pipeline had in flight
                 * happened in another session, and the first enable
                 * after a restore consumes whatever port A last
                 * fetched. The BRAM build gets this for free from its
                 * always-on read register.
                 *
                 * The address is taken from the word being jammed and
                 * not from cpu_addr, because cpu_addr is the flop the
                 * jam is writing on this very edge: reading it here
                 * fetches the address the core had BEFORE the restore,
                 * which after a reconfigure is zero. */
                rp6502_ram_a_addr = eng_st_jam ? eng_jam_w65c02[4][31:16]
                    : cpu_next_addr;
                rp6502_ram_a_wdata = cpu_next_data;
                rp6502_ram_a_we = cpu_next_we && !eng_st_jam;
                rp6502_ram_b_addr = eng_sram_sel
                    ? eng_sram_addr : bus_addr[15:0];
                rp6502_ram_b_wdata = eng_sram_sel
                    ? eng_sram_wdata : bus_wbyte;
                rp6502_ram_b_we = eng_sram_sel ? eng_sram_we
                    : (bus_we && !eng_arr_own);
                rp6502_ram_b_stb = eng_sram_sel
                    || (bus_pend && bus_sel_sram && !eng_arr_own);
                sram_rdata = ram_a_rdata;
                sram_b_rdata = ram_b_rdata;
            end
        end else begin : g_ram_bram
            sram64k sram (
                .clk(clk_sys),
                .sst_own(eng_arr_own),
                .sst_addr(eng_sram_addr),
                .sst_we(eng_sram_we),
                .sst_wdata(eng_sram_wdata),
                .a_addr(cpu_addr),
                .a_wdata(cpu_dout),
                .a_we(cpu_we && phi2_en),
                .a_rdata(sram_rdata),
                .b_addr(bus_addr[15:0]),
                .b_wdata(bus_wbyte),
                .b_we(bus_stb && bus_we && bus_sel_sram),
                .b_rdata(sram_b_rdata)
            );
            always_comb begin
                rp6502_ram_a_addr = '0;
                rp6502_ram_a_wdata = '0;
                rp6502_ram_a_we = 1'b0;
                rp6502_ram_b_addr = '0;
                rp6502_ram_b_wdata = '0;
                rp6502_ram_b_we = 1'b0;
                rp6502_ram_b_stb = 1'b0;
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
    logic [7:0] w65c22_data;
    w65c22 w65c22 (
        .clk(clk_mach),
        .rst_n(resb_eff),
        .en(phi2_en),
        .cs(sel_w65c22),
        .we(cpu_we),
        .rs(cpu_addr[3:0]),
        .data_i(cpu_dout),
        .w65c22_data(w65c22_data),
        .w65c22_irq(w65c22_irq),
        .st_idx(eng_st_idx),
        .w65c22_st_rdata(w65c22_st_rdata),
        .st_jam(eng_st_jam),
        .st_jam_data(eng_jam_w65c22)
    );

    /* The soft CPU's clock is half this one. Fixed here rather than in
     * rv_soc, which does not know it is clocked slowly: the strobe and
     * the console valid narrow to one machine clock, while slot_set and
     * key_set hold for two so an edge always sees them. */
    logic bus_stb_raw, bus_stb_n, bus_stb_q;
    logic rv_tx_valid_raw, rv_tx_valid_q;
    logic slot_set_q, key_set_q;

    /* The two clocks rise together, so a rising-edge sample of
     * bus_stb_raw compares it against a copy that caught the new value an
     * edge early: it reads 1 && !1, the pulse never fires, and the access
     * disappears silently. Which accesses go depends on skew between two
     * global networks, so it differs every fit and never in simulation.
     * On the falling edge the value has been still for half a period.
     *
     * Neither half takes a reset: asynchronous clear is a control signal
     * a LAB shares, and giving the two halves different control frees the
     * fitter to separate them. */
    initial bus_stb_n = 1'b0;
    always_ff @(negedge clk_sys) begin
        bus_stb_n <= bus_stb_raw;
    end
    initial begin
        bus_stb_q = 1'b0;
        rv_tx_valid_q = 1'b0;
        slot_set_q = 1'b0;
        key_set_q = 1'b0;
    end
    always_ff @(posedge clk_mach) begin
        bus_stb_q <= bus_stb_n;
        rv_tx_valid_q <= rv_tx_valid_raw;
        slot_set_q <= slot_set;
        key_set_q <= key_set;
    end
    /* Rising edge: rv_tx_valid_raw is a clean flop from the soft CPU's
     * clock with no term from this one, so a later sample only narrows
     * the pulse to a half period. */
    always_comb rp6502_rv_tx_valid = rv_tx_valid_raw && !rv_tx_valid_q;

    /* Two masters, and only ever one at a time. The soft CPU's accesses
     * arrive on its own slower clock and are narrowed to one system
     * clock by the pair of flops above; the engine already runs on this
     * clock and needs none of that, so the mux is after the narrowing
     * rather than in front of it. The engine only ever holds the bus
     * while the soft CPU is halted and issuing nothing. */
    logic bus_stb, bus_we, bus_pend;
    logic rv_bus_pend, rv_bus_we;
    logic [31:0] rv_bus_addr, rv_bus_wdata;
    logic [3:0] rv_bus_wstrb;
    /* One master again. The savestate serializer used to take this bus
     * over; it does not any more, because the bus is machine logic and
     * machine logic has no clock while a savestate is being made. It
     * reaches every array by a port of its own instead. */
    always_comb begin
        bus_pend = rv_bus_pend;
        bus_stb = bus_stb_n && !bus_stb_q;
        bus_we = rv_bus_we;
        bus_addr = rv_bus_addr;
        bus_wdata = rv_bus_wdata;
        bus_wstrb = rv_bus_wstrb;
    end

    /* Held until the request it answers goes away, because the other
     * clock looks only every second one. */
    logic bus_taken;
    initial bus_taken = 1'b0;
    always_ff @(posedge clk_mach) begin
        if (!bus_pend)
            bus_taken <= 1'b0;
        /* An XRAM access is taken on the clock it owns port B, which is
         * not always the clock it was strobed on. Everything else is
         * taken at the strobe as before. */
        else if (bus_sel_xram ? xram_go : bus_stb)
            bus_taken <= 1'b1;
    end
    logic [31:0] bus_addr, bus_wdata;
    logic [3:0] bus_wstrb;
    logic [31:0] bus_rdata;
    logic bus_rdy;

    /* RV_KHZ, not SYS_KHZ: mtime_acc is clocked by clk_rv, and a
     * microsecond is 25.2 of those. Ten per clock wrapping at a
     * hundredth of the rate keeps the fraction exact. */
    rv_soc #(
        .MTIME_ADD(10),
        .MTIME_WRAP(RV_KHZ / 100),
        .TCM_INIT_FILE(TCM_INIT_FILE)
    ) rv (
        .clk(clk_rv),
        .rst_n(rst_n),
        .rv_soc_phi2_khz(phi2_khz),
        /* Time stops with the machine. A savestate is meant to be
         * invisible to the firmware, and a firmware that woke to find
         * its deadlines already past would notice at once. */
        .sst_dbg_halt(sst_dbg_halt || eng_dbg_halt),
        .sst_dbg_halt_on_reset(sst_dbg_halt_on_reset),
        .sst_dbg_resume(sst_dbg_resume || eng_dbg_resume),
        .rv_soc_dbg_halted(rp6502_sst_dbg_halted),
        .sst_dbg_data0(eng_own ? eng_dbg_data0 : sst_dbg_data0),
        .rv_soc_dbg_data0(rp6502_sst_dbg_data0),
        .rv_soc_dbg_data0_wen(rp6502_sst_dbg_data0_wen),
        .sst_dbg_instr(eng_own ? eng_dbg_instr : sst_dbg_instr),
        .sst_dbg_instr_vld(eng_own ? eng_dbg_vld : sst_dbg_instr_vld),
        .rv_soc_dbg_instr_rdy(rp6502_sst_dbg_instr_rdy),
        .rv_soc_dbg_ebreak(rp6502_sst_dbg_ebreak),
        .rv_soc_dbg_fault(rp6502_sst_dbg_fault),
        .sst_phi2_we(eng_st_jam),
        .sst_phi2_wdata(eng_jam_mach[1][15:0]),
        .rv_soc_mtime(mtime),
        .sst_mtime_we(eng_mtime_jam),
        .sst_mtime_wdata({eng_jam_mach[3], eng_jam_mach[2]}),
        .sst_tcm_sel(eng_own ? eng_tcm_sel : sst_tcm_sel),
        .sst_tcm_addr(eng_own ? eng_tcm_addr : sst_tcm_addr),
        .sst_tcm_we(eng_own ? eng_tcm_we : sst_tcm_we),
        .sst_tcm_wdata(eng_own ? eng_tcm_wdata : sst_tcm_wdata),
        .rv_soc_tcm_rdata(rp6502_sst_tcm_rdata),
        .rv_soc_tx_data(rp6502_rv_tx_data),
        .rv_soc_tx_valid(rv_tx_valid_raw),
        .rv_soc_halted(rp6502_rv_halted),
        .rv_soc_exit_code(rp6502_rv_exit_code),
        .slot_set(slot_set || slot_set_q),
        .slot_len(slot_len),
        .upd_n(upd_n),
        .key_set(key_set || key_set_q),
        .cont_key(cont_key),
        .cont_joy(cont_joy),
        .cont_trig(cont_trig),
        .key_code(key_code),
        .rv_soc_key_pending(rp6502_key_pending),
        .bus_rdy(bus_rdy),
        .bus_taken(bus_taken),
        .rv_soc_bus_pend(rv_bus_pend),
        .rv_soc_bus_stb(bus_stb_raw),
        .rv_soc_bus_we(rv_bus_we),
        .rv_soc_bus_addr(rv_bus_addr),
        .rv_soc_bus_wdata(rv_bus_wdata),
        .rv_soc_bus_wstrb(rv_bus_wstrb),
        .bus_rdata(bus_rdata)
    );

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_bus;
    always_comb unused_bus = ^{bus_addr[27:16]};
    /* verilator lint_on UNUSEDSIGNAL */
    logic [7:0] bus_wbyte;
    always_comb begin
        bus_wbyte = bus_wdata[7:0];
        if (bus_wstrb[1])
            bus_wbyte = bus_wdata[15:8];
        if (bus_wstrb[2])
            bus_wbyte = bus_wdata[23:16];
        if (bus_wstrb[3])
            bus_wbyte = bus_wdata[31:24];
    end

    logic bus_sel_sram, bus_sel_regs, bus_sel_ctl, bus_sel_stage,
        bus_sel_vid, bus_sel_xram, bus_sel_aud, bus_sel_host;
    always_comb begin
        bus_sel_sram = bus_addr[31:28] == 4'h1;
        bus_sel_xram = bus_addr[31:28] == 4'h3;
        bus_sel_regs = bus_addr[31:28] == 4'h2;
        bus_sel_ctl = bus_addr[31:28] == 4'h4;
        bus_sel_stage = bus_addr[31:28] == 4'h6;
        bus_sel_vid = bus_addr[31:28] == 4'h5;
        bus_sel_aud = bus_addr[31:28] == 4'h7;
        bus_sel_host = bus_addr[31:28] == 4'h8;
    end

    always_comb begin
        rp6502_host_addr = bus_addr[27:0];
        rp6502_host_stb = bus_stb && bus_sel_host;
        rp6502_host_we = bus_we;
        rp6502_host_wdata = bus_wdata;
    end

    /* Shown early for a slow platform; the strobe-captured register
     * holds it through the answer cycle. */
    logic [27:0] stage_addr_q;
    always_comb begin
        /* The serializer reads the store directly; it is the board's
         * memory, not the machine's, and it keeps its clock when the
         * machine loses one. */
        rp6502_stage_pend = eng_stage_pend || (bus_pend && bus_sel_stage);
        rp6502_stage_addr = eng_stage_pend ? eng_stage_addr
            : ((bus_pend && bus_sel_stage) ? bus_addr[27:0] : stage_addr_q);
    end

    logic api_pending;
    logic bus_ctl_api, bus_vid_prog;
    logic [31:0] regs_b_rdata, regs_b_q;
    logic [31:0] vid_b_rdata;
    // Which target answers: 0 sram, 1 regs, 2 control, 3 staging, 4 vid,
    // 5 xram, 6 the platform's own.
    logic [2:0] bus_rsel;
    initial begin
        bus_rsel = 3'd0;
        bus_ctl_api = 1'b0;
        bus_vid_prog = 1'b0;
        stage_addr_q = '0;
    end
    always_ff @(posedge clk_mach) begin
        if (bus_stb) begin
            bus_rsel <= bus_sel_regs ? 3'd1
                : (bus_sel_ctl ? 3'd2
                : (bus_sel_stage ? 3'd3
                : (bus_sel_vid ? 3'd4
                : (bus_sel_xram ? 3'd5
                : (bus_sel_host ? 3'd6 : 3'd0)))));
            bus_ctl_api <= bus_addr[2];
            bus_vid_prog <= bus_addr[17];
            stage_addr_q <= bus_addr[27:0];
            /* Captured at the strobe: ring reads advance their pointer
             * there, so the answer must not be re-derived afterward. */
            regs_b_q <= regs_b_rdata;
        end
    end

    /* Held at power-on and the firmware's from then on. The platform's
     * reset does not reach it: cpu_init drives it low first.
     *
     * A restore puts it back before anything else, because it is what
     * holds the 6502 and the VIA in reset and a register jammed into a
     * part in reset is gone by the next clock. Waking is a reconfigure,
     * so it always comes up low and the blob is the only thing that
     * knows any different. */
    initial resb = 1'b0;
    always_ff @(posedge clk_mach)
        if (eng_st_jam) resb <= eng_jam_mach[0][0];
        else if (bus_stb && bus_we && bus_sel_ctl && !bus_addr[2])
            resb <= bus_wbyte[0];

    /* The byte-wide windows put their byte on every lane, so the master's
     * own extract picks the addressed one. */
    logic [7:0] bus_rbyte;
    always_comb begin
        case (bus_rsel)
            3'd2: bus_rbyte = bus_ctl_api ? {7'b0, api_pending}
                : {6'b0, cpu_stp, resb_eff};
            3'd3: bus_rbyte = stage_rdata;
            3'd5: bus_rbyte = xram_b_hold;
            default: bus_rbyte = sram_b_rdata;
        endcase
        bus_rdata = bus_rsel == 3'd1 ? regs_b_q
            : (bus_rsel == 3'd4
               ? (bus_vid_prog ? vid_prog_b_rdata : vid_b_rdata)
               : (bus_rsel == 3'd6 ? host_rdata : {4{bus_rbyte}}));
    end

    logic [7:0] ria_data;
    ria_regs ria (
        .clk(clk_mach),
        .clk_mem(clk_sys),
        .en(phi2_en),
        .cs(sel_ria),
        .we(cpu_we),
        .rs(cpu_addr[4:0]),
        .data_i(cpu_dout),
        .ria_regs_data(ria_data),
        .ria_regs_tx_data(rp6502_tx_data),
        .ria_regs_tx_valid(rp6502_tx_valid),
        .rx_valid(rx_valid),
        .rx_data(rx_data),
        .ria_regs_rx_taken(rp6502_rx_taken),
        .vsync_pulse(prog_vsync_pulse),
        .ria_regs_irq(ria_irq),
        .ria_regs_xr_busy(xr_busy),
        .ria_regs_xr_we(xr_we),
        .ria_regs_xr_addr(xr_addr),
        .ria_regs_xr_wdata(xr_wdata),
        .xr_rdata(xram_b_rdata),
        .xr_cpu_want(bus_pend && bus_sel_xram),
        .sst_jam(eng_st_jam),
        .sst_jam_data(eng_jam_ria),
        .sst_own(eng_arr_own),
        .sst_word(eng_regs_word),
        .sst_we(eng_regs_we),
        .sst_wdata(eng_regs_wdata),
        .b_we(bus_stb && bus_we && bus_sel_regs),
        .b_re(bus_stb && !bus_we && bus_sel_regs),
        .b_word(bus_addr[9:2]),
        .b_wstrb(bus_wstrb),
        .b_wdata(bus_wdata),
        .ria_regs_b_rdata(regs_b_rdata),
        .ria_regs_api_pending(api_pending),
        .api_ack(bus_stb && bus_we && bus_sel_ctl && bus_addr[2])
    );

    /* The bus keeps its last value across the unmapped window. */
    logic [7:0] bus_hold;
    always_comb begin
        if (sel_ria)
            cpu_din = ria_data;
        else if (sel_w65c22)
            cpu_din = w65c22_data;
        else if (sel_open)
            cpu_din = bus_hold;
        else
            cpu_din = sram_rdata;
    end

    initial bus_hold = 8'h00;
    always_ff @(posedge clk_mach)
        if (phi2_en && !cpu_we)
            bus_hold <= cpu_din;

    logic [9:0] vid_h /*verilator public_flat_rd*/;
    logic [9:0] vid_v /*verilator public_flat_rd*/;
    logic vid_de_full;
    logic vid_de /*verilator public_flat_rd*/;
    logic vid_hsync /*verilator public_flat_rd*/;
    logic vid_vsync /*verilator public_flat_rd*/;
    logic vid_line_start, vid_frame_start;
    always_comb rp6502_vid_frame = vid_frame_start;
    logic vid_vsync_pulse;
    logic prog_vsync_pulse;
    logic vid_px_first, vid_px_last;
    vid_timing vid_timing (
        .clk(clk_mach),
        .vid_timing_h(vid_h),
        .vid_timing_v(vid_v),
        .vid_timing_px_first(vid_px_first),
        .vid_timing_px_last(vid_px_last),
        .vid_timing_de(vid_de_full),
        .vid_timing_hsync(vid_hsync),
        .vid_timing_vsync(vid_vsync),
        .vid_timing_line_start(vid_line_start),
        .vid_timing_frame_start(vid_frame_start),
        .vid_timing_vsync_pulse(vid_vsync_pulse)
    );
    always_comb rp6502_scanline = vid_v;

    /* Port B is the RW engine's while busy; its background refresh
     * yields to a waiting soft CPU, but a real 6502 access does not.
     * See xram_owed below for what that costs and how it is handled. */
    logic [7:0] xram_b_rdata;
    logic xr_busy, xr_we;
    logic [15:0] xr_addr;
    logic [7:0] xr_wdata;
    logic [31:0] xram_a_rdata;
    /* The terminal and a mode-1 fill share the font store, so the rotor
     * is real arbitration. Mod two, so the wrap is free. */
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
    vid_font vid_font (
        .clk(clk_mach),
        .addr(mf_addr[f_sel]),
        .vid_font_bits(font_bits),
        .w_stb(bus_stb && bus_we && bus_sel_vid && bus_addr[18]),
        .w_addr(bus_addr[13:0]),
        .w_data(bus_wdata)
    );

    /* The two renderers. Mod two, so the wrap is free. */
    logic [1:0] ma_req;
    logic [13:0] ma_addr[2];
    logic a_rotor, a_sel;
    logic a_any;
    always_comb begin
        a_sel = a_rotor;
        a_any = 1'b0;
        for (int i = 0; i < 2; i++) begin
            logic cand;
            cand = a_rotor ^ 1'(i);
            if (!a_any && ma_req[cand]) begin
                a_sel = cand;
                a_any = 1'b1;
            end
        end
    end
    initial a_rotor = 1'd0;
    always_ff @(posedge clk_mach)
        if (a_any)
            a_rotor <= a_sel + 1'd1;

    /* bus_rdy holds the soft CPU's strobe off while the engine has port
     * B, but the strobe is two flops behind the readiness it was granted
     * on -- the narrowing above -- so the engine can still take the port
     * on the clock the strobe lands, and under a program working RW0 it
     * usually does. bus_rdy alone therefore decides nothing: the access
     * would be reported complete either way, with the write dropped and
     * the read answering from whatever address the engine was at.
     *
     * So it waits for the port instead of assuming it. One bit is
     * enough, because the master issues nothing else until bus_taken. */
    logic xram_owed;
    initial xram_owed = 1'b0;
    always_ff @(posedge clk_mach) begin
        if (!bus_pend)
            xram_owed <= 1'b0;
        else if (bus_stb && bus_sel_xram && xr_busy)
            xram_owed <= 1'b1;
        else if (xram_owed && !xr_busy)
            xram_owed <= 1'b0;
    end
    logic xram_go;
    always_comb xram_go = !xr_busy
        && ((bus_stb && bus_sel_xram) || xram_owed);

    /* Port B's write, named because the PSG watches it. The RW engine's
     * is the 6502's, and only the 6502's strikes a gate. */
    logic xw_we, xw_host;
    logic [15:0] xw_addr;
    logic [7:0] xw_wdata;
    always_comb begin
        xw_host = xr_busy;
        /* The xr term's mask is the one write in the machine that
         * neither reset nor the jam reaches: the RW engine has no
         * reset, so an op suspended by the clock cut resumes when the
         * clock returns and would land one stale byte in freshly
         * restored XRAM. It is old-session work; it is discarded. */
        xw_we = xr_busy ? (xr_we && !eng_arr_own && !eng_hold_res)
                        : (xram_go && bus_we && !eng_arr_own);
        xw_addr = xr_busy ? xr_addr : bus_addr[15:0];
        xw_wdata = xr_busy ? xr_wdata : bus_wbyte;
    end

    /* The array answers a clock behind its address and the engine may
     * own it by then, so the byte is caught on the clock after the one
     * the access owned. The regs window is held for the same reason a
     * few hundred lines up; this one is a clock later because that read
     * is combinational and this one is not. */
    logic xram_cap;
    logic [7:0] xram_b_hold;
    initial begin
        xram_cap = 1'b0;
        xram_b_hold = '0;
    end
    always_ff @(posedge clk_mach) begin
        xram_cap <= xram_go && !bus_we;
        if (xram_cap)
            xram_b_hold <= xram_b_rdata;
    end
    /* A clock behind, because the soft CPU's half of that mux is
     * combinational off its bus and reaches the byte the envelope's
     * comparators are built on. Unregistered it ran a foreign bus through
     * the deepest arithmetic in the machine. The engine reads no XRAM, so
     * all the extra clock moves is when a gate strikes. */
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
    xram64k xram (
        .clk(clk_sys),
        .sst_own(eng_arr_own),
        .sst_addr(eng_mem_addr),
        .sst_we(eng_xram_we),
        .sst_wdata(eng_mem_wdata),
        .a_addr(ma_addr[a_sel]),
        .xram64k_a_rdata(xram_a_rdata),
        .b_addr(xw_addr),
        .b_wdata(xw_wdata),
        .b_we(xw_we),
        .xram64k_b_rdata(xram_b_rdata)
    );

    logic [31:0] vid_prog_b_rdata;
    logic [2:0] vid_canvas;
    always_comb rp6502_vid_canvas = vid_canvas;
    logic [9:0] vid_cw, vid_ch;

    logic [8:0] sched_p_line;
    logic [1:0] sched_p_plane;
    logic [31:0] pm_entry;
    logic [15:0] pm_config;

    vid_prog vid_prog (
        .clk(clk_mach),
        .clk_mem(clk_sys),
        .frame_start(vid_frame_start),
        .v(vid_v),
        .px_first(vid_px_first),
        .vid_prog_vsync_pulse(prog_vsync_pulse),
        .h(vid_h),
        .vid_prog_canvas(vid_canvas),
        .vid_prog_cw(vid_cw),
        .vid_prog_ch(vid_ch),
        .p_line(sched_p_line),
        .p_plane(sched_p_plane),
        .vid_prog_p_entry(pm_entry),
        .vid_prog_p_config(pm_config),
        .s_idx(sp_s_idx),
        .vid_prog_s_data(sp_s_data),
        .sst_own(eng_arr_own),
        .sst_addr(eng_mem_addr[10:0]),
        .sst_word(eng_xprog_word),
        .sst_we(eng_xprog_we),
        .sst_wdata(eng_mem_wdata),
        .vid_prog_sst_rdata(eng_xprog_rdata),
        .b_stb(bus_stb && bus_sel_vid && !bus_addr[18]
               && bus_addr[17]),
        .b_we(bus_we),
        .b_addr(bus_addr[15:0]),
        .b_wdata(bus_wdata),
        .vid_prog_b_rdata(vid_prog_b_rdata)
    );

    logic [15:0] mode0_pix;
    vid_mode0 vid_mode0 (
        .clk(clk_mach),
        .clk_mem(clk_sys),
        .frame_start(vid_frame_start),
        .h(vid_h),
        .v(vid_v),
        .px_last(vid_px_last),
        .line_start(vid_line_start),
        .cw(vid_cw),
        .vid_mode0_pix(mode0_pix),
        .vid_mode0_f_req(mf_req[1]),
        .vid_mode0_f_addr(mf_addr[1]),
        .f_gnt(f_any && f_sel == 1'd1),
        .f_data(font_bits),
        .sst_own(eng_arr_own),
        .sst_addr(eng_mem_addr),
        .sst_we(eng_cell_we),
        .sst_wdata(eng_mem_wdata),
        .vid_mode0_sst_rdata(eng_cell_rdata),
        .b_stb(bus_stb && bus_sel_vid && !bus_addr[18]
               && !bus_addr[17]),
        .b_we(bus_we),
        .b_addr(bus_addr[16:0]),
        .b_wstrb(bus_wstrb),
        .b_wdata(bus_wdata),
        .vid_mode0_b_rdata(vid_b_rdata)
    );

    logic [15:0] m_pix[3];
    logic [12:0] sp_s_idx;
    logic [31:0] sp_s_data;
    logic [16:0] sp_pix[3];

    logic fl_start;
    logic [2:0] fl_mode;
    logic [15:0] fl_attr, fl_config;
    logic fl_done;
    logic fl_px_we;
    logic [9:0] fl_px_addr;
    logic [15:0] fl_px_data;
    logic [2:0] m_px_we;
    logic [2:0] m_done;
    logic [2:0] sched_term;
    vid_sched vid_sched (
        .clk(clk_mach),
        .rst_n(rst_n),
        .v(vid_v),
        .h(vid_h),
        .line_start(vid_line_start),
        .cw(vid_cw),
        .ch(vid_ch),
        .vid_sched_p_line(sched_p_line),
        .vid_sched_p_plane(sched_p_plane),
        .p_entry(pm_entry),
        .p_config(pm_config),
        .vid_sched_e_start(fl_start),
        .vid_sched_e_mode(fl_mode),
        .vid_sched_e_attr(fl_attr),
        .vid_sched_e_config(fl_config),
        .e_done(fl_done),
        .e_px_we(fl_px_we),
        .vid_sched_px_we(m_px_we),
        .vid_sched_done(m_done),
        .vid_sched_term(sched_term)
    );
    vid_fill vid_fill (
        .clk(clk_mach),
        .line_start(vid_line_start),
        .start(fl_start),
        .mode(fl_mode),
        .attr_i(fl_attr),
        .config_ptr_i(fl_config),
        .t_row(sched_p_line),
        .cw(vid_cw),
        .vid_fill_a_req(ma_req[0]),
        .vid_fill_a_addr(ma_addr[0]),
        .a_gnt(a_any && a_sel == 1'd0),
        .a_rdata(xram_a_rdata),
        .vid_fill_f_req(mf_req[0]),
        .vid_fill_f_addr(mf_addr[0]),
        .f_gnt(f_any && f_sel == 1'd0),
        .f_data(font_bits),
        .vid_fill_px_we(fl_px_we),
        .vid_fill_px_addr(fl_px_addr),
        .vid_fill_px_data(fl_px_data),
        .vid_fill_done(fl_done)
    );
    genvar gi;
    generate
        for (gi = 0; gi < 3; gi++) begin : gen_mode
            vid_mode vid_mode (
                .clk(clk_mach),
                .h(vid_h),
                .px_last(vid_px_last),
                .line_start(vid_line_start),
                .px_we(m_px_we[gi]),
                .px_addr(fl_px_addr),
                .px_data(fl_px_data),
                .done_i(m_done[gi]),
                .vid_mode_pix(m_pix[gi])
            );
        end
    endgenerate

    /* verilator lint_off PINCONNECTEMPTY */
    vid_sprite vid_sprite (
        .clk(clk_mach),
        .v(vid_v),
        .h(vid_h),
        .px_last(vid_px_last),
        .line_start(vid_line_start),
        .cw(vid_cw),
        .ch(vid_ch),
        .vid_sprite_s_idx(sp_s_idx),
        .s_data(sp_s_data),
        .vid_sprite_pix(sp_pix),
        .vid_sprite_overrun(),
        .vid_sprite_a_req(ma_req[1]),
        .vid_sprite_a_addr(ma_addr[1]),
        .a_gnt(a_any && a_sel == 1'd1),
        .a_rdata(xram_a_rdata)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    /* Four bits of offset rather than two, so the bell's descriptor has
     * somewhere to live beside the two pointers. */
    logic aud_we;
    always_comb aud_we = bus_stb && bus_we && bus_sel_aud;

    logic psg_tick;

    logic signed [15:0] psg_l, psg_r;
    /* verilator lint_off PINCONNECTEMPTY */
    aud_psg aud_psg (
        .clk(clk_mach),
        .xaddr_we(aud_we && bus_addr[5:2] == 4'h0),
        .xaddr_wdata(bus_wdata[15:0]),
        .gate_any_we(aud_we && bus_addr[5:2] == 4'h1),
        .gate_any_wdata(bus_wdata[0]),
        .q_we(qs_we),
        .q_host(qs_host),
        .q_addr(qs_addr),
        .q_val(qs_val),
        .bel_lo_we(aud_we && bus_addr[5:2] == 4'h4),
        .bel_hi_we(aud_we && bus_addr[5:2] == 4'h5),
        .bel_wdata(bus_wdata),
        .aud_psg_l(psg_l),
        .aud_psg_r(psg_r),
        /* The machine runs off the tick below, which is the divider and
         * not the walk. */
        .aud_psg_valid(),
        .aud_psg_tick(psg_tick)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    logic signed [15:0] opl_l;
    logic opl_valid;
    /* verilator lint_off PINCONNECTEMPTY */
    aud_opl aud_opl (
        .clk(clk_mach),
        .xaddr_we(aud_we && bus_addr[5:2] == 4'h2),
        .xaddr_wdata(bus_wdata[15:0]),
        /* The same mux the PSG watches, not the 6502's engine alone: a
         * restore puts the register page back in XRAM and the firmware
         * writes it over itself so the engine hears it, and those
         * writes are the soft CPU's. */
        .q_we(qs_we),
        .q_addr(qs_addr),
        .q_val(qs_val),
        .aud_opl_out(opl_l),
        .aud_opl_valid(opl_valid),
        .aud_opl_enabled()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    /* A YM3812 samples every 1014 clk_sys and the codec every 1050, so
     * this is the one voice whose rate is not ours to choose. One
     * instance, because a YM3812 is mono. */
    logic signed [15:0] opl_rs;
    /* verilator lint_off PINCONNECTEMPTY */
    aud_rsmp aud_rsmp (
        .clk(clk_mach),
        .in_sample(opl_l),
        .in_valid(opl_valid),
        .step(psg_tick),
        .aud_rsmp_out(opl_rs),
        /* Pulled, so its answer is ready when the tick comes round. */
        .aud_rsmp_valid()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    /* Nothing selects and nothing gates: an engine making no sound
     * contributes zero. One heartbeat, the PSG's divider — the tick, not
     * the end of a walk, whose length moves with the XRAM rotor. */
    logic signed [16:0] eng_l, eng_r;
    always_comb begin
        eng_l = 17'(opl_rs) + 17'(psg_l);
        eng_r = 17'(opl_rs) + 17'(psg_r);
        rp6502_aud_l = eng_l < -17'sd32768 ? -16'sd32768
            : eng_l > 17'sd32767 ? 16'sd32767 : 16'(eng_l);
        rp6502_aud_r = eng_r < -17'sd32768 ? -16'sd32768
            : eng_r > 17'sd32767 ? 16'sd32767 : 16'(eng_r);
        rp6502_aud_valid = psg_tick;
    end

    /* de IS the canvas: its rows on the first canvas_h lines, its own
     * width, and the scaler does the rest. */
    always_comb vid_de = vid_de_full && vid_h < vid_cw && vid_v < vid_ch;

    /* A mode-0 slot swaps the plane's fill stream for the terminal
     * engine's; sprites still merge on top. */
    logic [15:0] c_pix[3];
    always_comb
        for (int i = 0; i < 3; i++)
            c_pix[i] = sched_term[i] ? mode0_pix : m_pix[i];
    vid_compose vid_compose (
        .clk(clk_mach),
        .de(vid_de),
        .p0_pix(c_pix[0]),
        .s0_pix(sp_pix[0]),
        .p1_pix(c_pix[1]),
        .s1_pix(sp_pix[1]),
        .p2_pix(c_pix[2]),
        .s2_pix(sp_pix[2]),
        .vid_compose_pix(rp6502_vid_pixel),
        .vid_compose_de(rp6502_vid_de)
    );

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid;
    always_comb unused_vid = ^{vid_hsync, vid_vsync, vid_vsync_pulse};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
