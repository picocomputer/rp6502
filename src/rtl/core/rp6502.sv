/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

module rp6502
    import rp6502_pkg::*;
#(
    parameter TCM_INIT_FILE = "",
    parameter int SYS_KHZ = 50400,
    parameter bit EXT_RAM = 0
) (
    input logic clk_sys,
    input logic clk_mach,
    input logic clk_rv,
    input logic rst_n,

    output logic [7:0] rp6502_tx_data,
    output logic rp6502_tx_valid,
    input logic rx_valid,
    input logic [7:0] rx_data,
    output logic rp6502_rx_taken,

    output logic [7:0] rp6502_rv_tx_data,
    output logic rp6502_rv_tx_valid,
    input logic rv_tx_full,
    output logic rp6502_rv_halted,
    output logic [31:0] rp6502_rv_exit_code,

    output logic [27:0] rp6502_stage_addr,
    output logic rp6502_stage_pend,
    input logic stage_stall,
    input logic [7:0] stage_rdata,

    input logic mach_running,
    output logic rp6502_sst_stop_req,
    input logic sst_tcm_sel,
    input logic [14:0] sst_tcm_addr,
    input logic sst_tcm_we,
    input logic [31:0] sst_tcm_wdata,
    output logic [31:0] rp6502_sst_tcm_rdata,

    input logic sst_save,
    input logic sst_load,
    output logic rp6502_sst_load_done,
    output logic rp6502_sst_load_err,
    output logic rp6502_sst_ready,
    input logic [17:0] sst_rd_idx,
    input logic sst_rd_t,
    output logic [31:0] rp6502_sst_rdata,
    output logic rp6502_sst_rvalid,
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

    output logic [27:0] rp6502_host_addr,
    output logic rp6502_host_stb,
    output logic rp6502_host_we,
    output logic [31:0] rp6502_host_wdata,
    input logic [31:0] host_rdata,

    input logic slot_set,
    input logic [31:0] slot_len,
    input logic [7:0] upd_n,
    input logic key_set,
    input logic [8:0] key_code,
    input logic [3:0][31:0] cont_key,
    input logic [3:0][31:0] cont_joy,
    input logic [3:0][15:0] cont_trig,
    output logic rp6502_key_pending,

    output logic [15:0] rp6502_vid_pixel,
    output logic rp6502_vid_de,
    output logic [2:0] rp6502_vid_canvas,

    output logic signed [15:0] rp6502_aud_l,
    output logic signed [15:0] rp6502_aud_r,
    output logic rp6502_aud_valid,

    output logic [RP6502_SCANLINE_W-1:0] rp6502_scanline,
    output logic rp6502_vid_frame,

    output logic [15:0] rp6502_ram_a_addr,
    output logic [7:0] rp6502_ram_a_wdata,
    output logic rp6502_ram_a_we,
    input logic [7:0] ram_a_rdata,
    output logic [15:0] rp6502_ram_b_addr,
    output logic [7:0] rp6502_ram_b_wdata,
    output logic rp6502_ram_b_we,
    output logic rp6502_ram_b_stb,
    output logic rp6502_ram_refill,
    input logic [7:0] ram_b_rdata,
    input logic ram_b_stall,
    input logic ram_hold,
    output logic rp6502_phi2_en,
    output logic rp6502_cpu_run
);

    localparam int RV_KHZ = SYS_KHZ / 2;

    logic [15:0] phi2_khz;
    logic phi2_raw_en, phi2_en;
    always_comb bus_rdy = !(bus_sel_xram && xr_busy)
        && !(bus_sel_stage && stage_stall)
        && !(bus_sel_sram && ram_b_stall);

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
    logic via_irq;

    logic resb ;
    logic resb_eff;
    always_comb resb_eff = resb && !eng_hold_res;
    always_comb rp6502_cpu_run = resb_eff;
    logic cpu_stp;
    logic eng_st_jam, eng_mtime_jam;
    logic [63:0] mtime;
    logic [31:0] eng_jam_mach[4], eng_jam_cpu[5], eng_jam_via[7];
    logic [31:0] eng_jam_ria[12];

    localparam logic [1:0] SEL_MACH = 2'd0;
    localparam logic [1:0] SEL_CPU = 2'd1;

    logic [31:0] cpu65_st_rdata, via_st_rdata, mach_st_rdata, st_rdata;
    always_comb begin
        case (eng_st_idx)
            3'd0: mach_st_rdata = {31'd0, resb};
            3'd1: mach_st_rdata = {16'd0, phi2_khz};
            3'd2: mach_st_rdata = mtime[31:0];
            3'd3: mach_st_rdata = mtime[63:32];
            default: mach_st_rdata = '0;
        endcase
        case (eng_st_sel)
            SEL_MACH: st_rdata = mach_st_rdata;
            SEL_CPU:  st_rdata = cpu65_st_rdata;
            default:  st_rdata = via_st_rdata;
        endcase
    end

    cpu65 cpu (
        .clk(clk_mach),
        .rst_n(resb_eff),
        .en(phi2_en),
        .data_i(cpu_din),
        .irq_i(via_irq || ria_irq),
        .nmi_i(1'b0),
        .rdy_i(1'b0),
        .res_i(1'b0),
        .cpu65_addr(cpu_addr),
        .cpu65_data(cpu_dout),
        .cpu65_we(cpu_we),
        .cpu65_stp(cpu_stp),
        .st_idx(eng_st_idx),
        .cpu65_st_rdata(cpu65_st_rdata),
        .st_jam(eng_st_jam),
        .st_jam_data(eng_jam_cpu),
        .cpu65_next_addr(cpu_next_addr),
        .cpu65_next_data(cpu_next_data),
        .cpu65_next_we(cpu_next_we)
    );

    logic eng_freeze;
    always_comb rp6502_sst_stop_req = eng_freeze;
    always_comb rp6502_ram_refill = eng_st_jam;

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
        .sst_engine_jam_cpu(eng_jam_cpu),
        .sst_engine_jam_via(eng_jam_via),
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

    logic sel_via, sel_ria, sel_open;
    always_comb begin
        sel_via = cpu_addr[15:4] == 12'hFFD;
        sel_ria = cpu_addr[15:5] == 11'b1111_1111_111;
        sel_open = cpu_addr[15:8] == 8'hFF && !sel_via && !sel_ria;
    end

    logic [7:0] sram_rdata;
    logic [7:0] sram_b_rdata;
    generate
        if (EXT_RAM) begin : g_ram_ext
            always_comb begin
                rp6502_ram_a_addr = eng_st_jam ? eng_jam_cpu[4][31:16]
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
            logic unused_ram;
            always_comb unused_ram = ^{ram_a_rdata, ram_b_rdata,
                                       cpu_next_addr, cpu_next_data,
                                       cpu_next_we, eng_sram_sel};
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

    logic bus_stb_raw, bus_stb_n, bus_stb_q;
    logic rv_tx_valid_raw, rv_tx_valid_q;
    logic slot_set_q, key_set_q;

    initial bus_stb_n = 1'b0;
    always_ff @(negedge clk_mach) begin
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
    always_comb rp6502_rv_tx_valid = rv_tx_valid_raw && !rv_tx_valid_q;

    logic bus_stb, bus_we, bus_pend;
    logic rv_bus_pend, rv_bus_we;
    logic [31:0] rv_bus_addr, rv_bus_wdata;
    logic [3:0] rv_bus_wstrb;
    always_comb begin
        bus_pend = rv_bus_pend;
        bus_stb = bus_stb_n && !bus_stb_q;
        bus_we = rv_bus_we;
        bus_addr = rv_bus_addr;
        bus_wdata = rv_bus_wdata;
        bus_wstrb = rv_bus_wstrb;
    end

    logic [7:0] xram_b_q;
    logic xram_b_due;
    initial begin
        xram_b_q = 8'h00;
        xram_b_due = 1'b0;
    end
    always_ff @(posedge clk_mach) begin
        xram_b_due <= bus_stb && bus_sel_xram && !bus_we;
        if (xram_b_due) xram_b_q <= xram_b_rdata;
    end

    logic bus_taken;
    initial bus_taken = 1'b0;
    always_ff @(posedge clk_mach) begin
        if (!bus_pend)
            bus_taken <= 1'b0;
        else if (bus_stb)
            bus_taken <= 1'b1;
    end
    logic [31:0] bus_addr, bus_wdata;
    logic [3:0] bus_wstrb;
    logic [31:0] bus_rdata;
    logic bus_rdy;

    rv_soc #(
        .MTIME_ADD(10),
        .MTIME_WRAP(RV_KHZ / 100),
        .TCM_INIT_FILE(TCM_INIT_FILE)
    ) rv (
        .clk(clk_rv),
        .rst_n(rst_n),
        .rv_soc_phi2_khz(phi2_khz),
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
        .tx_full(rv_tx_full),
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

    logic unused_bus;
    always_comb unused_bus = ^{bus_addr[27:16]};
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

    logic [27:0] stage_addr_q;
    always_comb begin
        rp6502_stage_pend = eng_stage_pend || (bus_pend && bus_sel_stage);
        rp6502_stage_addr = eng_stage_pend ? eng_stage_addr
            : ((bus_pend && bus_sel_stage) ? bus_addr[27:0] : stage_addr_q);
    end

    logic api_pending;
    logic bus_ctl_api, bus_vid_prog;
    logic [31:0] regs_b_rdata, regs_b_q;
    logic [31:0] vid_b_rdata;
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
            regs_b_q <= regs_b_rdata;
        end
    end

    initial resb = 1'b0;
    always_ff @(posedge clk_mach)
        if (eng_st_jam) resb <= eng_jam_mach[0][0];
        else if (bus_stb && bus_we && bus_sel_ctl && !bus_addr[2])
            resb <= bus_wbyte[0];

    logic [7:0] bus_rbyte;
    always_comb begin
        case (bus_rsel)
            3'd2: bus_rbyte = bus_ctl_api ? {7'b0, api_pending}
                : {6'b0, cpu_stp, resb_eff};
            3'd3: bus_rbyte = stage_rdata;
            3'd5: bus_rbyte = xram_b_q;
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

    logic [7:0] bus_hold;
    always_comb begin
        if (sel_ria)
            cpu_din = ria_data;
        else if (sel_via)
            cpu_din = via_data;
        else if (sel_open)
            cpu_din = bus_hold;
        else
            cpu_din = sram_rdata;
    end

    initial bus_hold = 8'h00;
    always_ff @(posedge clk_mach)
        if (phi2_en && !cpu_we)
            bus_hold <= cpu_din;

    logic [9:0] vid_h ;
    logic [9:0] vid_v ;
    logic vid_de_full;
    logic vid_de ;
    logic vid_hsync ;
    logic vid_vsync ;
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

    logic [7:0] xram_b_rdata;
    logic xr_busy, xr_we;
    logic [15:0] xr_addr;
    logic [7:0] xr_wdata;
    logic [31:0] xram_a_rdata;
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

    logic xw_we, xw_host;
    logic [15:0] xw_addr;
    logic [7:0] xw_wdata;
    always_comb begin
        xw_host = xr_busy;
        xw_we = xr_busy ? (xr_we && !eng_arr_own && !eng_hold_res)
                        : (bus_stb && bus_we && bus_sel_xram
                           && !eng_arr_own);
        xw_addr = xr_busy ? xr_addr : bus_addr[15:0];
        xw_wdata = xr_busy ? xr_wdata : bus_wbyte;
    end
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

    logic aud_we;
    always_comb aud_we = bus_stb && bus_we && bus_sel_aud;

    logic psg_tick;

    logic signed [15:0] psg_l, psg_r;
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
        .aud_psg_valid(),
        .aud_psg_tick(psg_tick)
    );

    logic signed [15:0] opl_l;
    logic opl_valid;
    aud_opl aud_opl (
        .clk(clk_mach),
        .xaddr_we(aud_we && bus_addr[5:2] == 4'h2),
        .xaddr_wdata(bus_wdata[15:0]),
        .q_we(qs_we),
        .q_addr(qs_addr),
        .q_val(qs_val),
        .aud_opl_out(opl_l),
        .aud_opl_valid(opl_valid),
        .aud_opl_enabled()
    );

    logic signed [15:0] opl_rs;
    aud_rsmp aud_rsmp (
        .clk(clk_mach),
        .in_sample(opl_l),
        .in_valid(opl_valid),
        .step(psg_tick),
        .aud_rsmp_out(opl_rs),
        .aud_rsmp_valid()
    );

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

    always_comb vid_de = vid_de_full && vid_h < vid_cw && vid_v < vid_ch;

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

    logic unused_vid;
    always_comb unused_vid = ^{vid_hsync, vid_vsync, vid_vsync_pulse};

endmodule
