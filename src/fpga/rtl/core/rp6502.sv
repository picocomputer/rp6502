/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The RP6502 machine, independent of the FPGA platform hosting it. Platform
 * wrappers under platform/ adapt this to the Analogue Pocket (APF) or MiSTer;
 * the simulation verilates this module directly and drives it from sim/.
 *
 * So far: the 6502 at a divided PHI2, its 64 KB, the VIA at $FFD0, the
 * RIA's bare UART and register cells at $FFE0, and the soft CPU that owns
 * them all — a Hazard3 whose firmware loads programs into the 6502's memory,
 * writes its vectors, and releases its reset, the way the RIA boots the real
 * machine. The memory map and its quirks follow emu/sys/mem.c: every write
 * also lands in the SRAM shadow, and $FF00-$FFCF reads float at the last
 * value the bus carried.
 *
 * The soft CPU's window on the machine, byte-wide unless noted:
 *   0x10000000  the 6502's 64 KB
 *   0x20000000  the RIA register cells, one per byte address
 *   0x40000000  control: bit 0 runs the 6502 (its RESB, inverted)
 */

module rp6502
    import rp6502_pkg::*;
(
    input logic clk_sys,
    input logic rst_n,

    /* Console: TX bytes out of $FFE1, RX bytes offered toward $FFE2. */
    output logic [7:0] rp6502_tx_data,
    output logic rp6502_tx_valid,
    input logic rx_valid,
    input logic [7:0] rx_data,
    output logic rp6502_rx_taken,

    /* The soft CPU's own console and the testbench halt. */
    output logic [7:0] rp6502_rv_tx_data,
    output logic rp6502_rv_tx_valid,
    output logic rp6502_rv_halted,
    output logic [31:0] rp6502_rv_exit_code,

    output logic [RP6502_SCANLINE_W-1:0] rp6502_scanline
);

    /* PHI2, fixed until the soft CPU programs it. */
    logic phi2_en;
    phi2_div phi2_div (
        .clk(clk_sys),
        .rst_n(rst_n),
        .div_int(16'd4),
        .div_frac(8'd0),
        .phi2_div_en(phi2_en)
    );

    /* The 6502 and its bus, decoded per the machine's map. */
    logic [15:0] cpu_addr;
    logic [7:0] cpu_dout, cpu_din;
    logic cpu_we;
    logic via_irq;
    // Opcode-fetch marker; the debug tap will want it, nothing does yet.
    logic cpu_sync;
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_sync;
    /* verilator lint_on UNUSEDSIGNAL */
    always_comb unused_sync = cpu_sync;

    /* The 6502 runs when the OS says so; cpu_run is its inverted RESB. */
    logic cpu_run /*verilator public_flat_rw*/;

    cpu65 cpu (
        .clk(clk_sys),
        .rst_n(rst_n && cpu_run),
        .en(phi2_en),
        .data_i(cpu_din),
        .irq_i(via_irq),
        .nmi_i(1'b0),
        .rdy_i(1'b0),
        .res_i(1'b0),
        .cpu65_addr(cpu_addr),
        .cpu65_data(cpu_dout),
        .cpu65_we(cpu_we),
        .cpu65_sync(cpu_sync)
    );

    logic sel_via, sel_ria, sel_open;
    always_comb begin
        sel_via = cpu_addr[15:4] == 12'hFFD;
        sel_ria = cpu_addr[15:5] == 11'b1111_1111_111;
        sel_open = cpu_addr[15:8] == 8'hFF && !sel_via && !sel_ria;
    end

    /* Every write lands in the shadow, whatever else it hits. */
    logic [7:0] sram_rdata;
    sram64k sram (
        .clk(clk_sys),
        .a_addr(cpu_addr),
        .a_wdata(cpu_dout),
        .a_we(cpu_we && phi2_en),
        .a_rdata(sram_rdata),
        .b_addr(bus_addr[15:0]),
        .b_wdata(bus_wbyte),
        .b_we(bus_stb && bus_we && bus_sel_sram),
        .b_rdata(sram_b_rdata)
    );

    logic [7:0] via_data;
    via via (
        .clk(clk_sys),
        .rst_n(rst_n),
        .en(phi2_en),
        .cs(sel_via),
        .we(cpu_we),
        .rs(cpu_addr[3:0]),
        .data_i(cpu_dout),
        .via_data(via_data),
        .via_irq(via_irq)
    );

    /* The soft CPU and its window on the machine. */
    logic bus_stb, bus_we;
    logic [31:0] bus_addr, bus_wdata;
    logic [3:0] bus_wstrb;
    logic [31:0] bus_rdata;

    rv_soc rv (
        .clk(clk_sys),
        .rst_n(rst_n),
        .rv_soc_tx_data(rp6502_rv_tx_data),
        .rv_soc_tx_valid(rp6502_rv_tx_valid),
        .rv_soc_halted(rp6502_rv_halted),
        .rv_soc_exit_code(rp6502_rv_exit_code),
        .rv_soc_bus_stb(bus_stb),
        .rv_soc_bus_we(bus_we),
        .rv_soc_bus_addr(bus_addr),
        .rv_soc_bus_wdata(bus_wdata),
        .rv_soc_bus_wstrb(bus_wstrb),
        .bus_rdata(bus_rdata)
    );

    /* Byte lane per sb/lb; the window is byte-wide by design, so the wide
     * decode bits and the lane-zero strobe fold into the others. */
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_bus;
    always_comb unused_bus = ^{bus_addr[27:16], bus_wstrb[0]};
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

    logic bus_sel_sram, bus_sel_regs, bus_sel_ctl;
    always_comb begin
        bus_sel_sram = bus_addr[31:28] == 4'h1;
        bus_sel_regs = bus_addr[31:28] == 4'h2;
        bus_sel_ctl = bus_addr[31:28] == 4'h4;
    end

    logic [7:0] sram_b_rdata, regs_b_rdata;
    logic [1:0] bus_rsel;  // which target answers: 0 sram, 1 regs, 2 control
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n) begin
            cpu_run <= 1'b0;
            bus_rsel <= 2'd0;
        end else begin
            if (bus_stb)
                bus_rsel <= bus_sel_regs ? 2'd1 : (bus_sel_ctl ? 2'd2 : 2'd0);
            if (bus_stb && bus_we && bus_sel_ctl)
                cpu_run <= bus_wbyte[0];
        end
    end

    /* Reads answer one cycle after the strobe, the byte on every lane so
     * the master's own extract picks the addressed one. */
    logic [7:0] bus_rbyte;
    always_comb begin
        case (bus_rsel)
            2'd1: bus_rbyte = regs_b_rdata;
            2'd2: bus_rbyte = {7'b0, cpu_run};
            default: bus_rbyte = sram_b_rdata;
        endcase
        bus_rdata = {4{bus_rbyte}};
    end

    logic [7:0] ria_data;
    ria_regs ria (
        .clk(clk_sys),
        .rst_n(rst_n),
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
        .b_we(bus_stb && bus_we && bus_sel_regs),
        .b_rs(bus_addr[4:0]),
        .b_wdata(bus_wbyte),
        .ria_regs_b_rdata(regs_b_rdata)
    );

    /* The bus keeps its last value across the unmapped window. */
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

    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n)
            bus_hold <= 8'h00;
        else if (phi2_en && !cpu_we)
            bus_hold <= cpu_din;
    end

    /* The frame cadence the video system will grow from. */
    always_ff @(posedge clk_sys or negedge rst_n)
        if (!rst_n)
            rp6502_scanline <= '0;
        else if (rp6502_scanline == RP6502_SCANLINE_W'(RP6502_V_TOTAL - 1))
            rp6502_scanline <= '0;
        else
            rp6502_scanline <= rp6502_scanline + 1'b1;

endmodule
