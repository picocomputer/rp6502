/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The RP6502 machine, independent of the FPGA platform hosting it. Platform
 * wrappers under platform/ adapt this to the Analogue Pocket (APF) or MiSTer;
 * the simulation verilates this module directly and drives it from sim/.
 *
 * So far: the 6502 at a divided PHI2, its 64 KB, the VIA at $FFD0, and the
 * RIA's bare UART and register cells at $FFE0 — enough machine to run a
 * program that prints. The memory map and its quirks follow emu/sys/mem.c:
 * every write also lands in the SRAM shadow, and $FF00-$FFCF reads float at
 * the last value the bus carried.
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

    cpu65 cpu (
        .clk(clk_sys),
        .rst_n(rst_n),
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
        .b_addr(16'h0000),
        .b_wdata(8'h00),
        .b_we(1'b0),
        /* verilator lint_off PINCONNECTEMPTY */
        .b_rdata()
        /* verilator lint_on PINCONNECTEMPTY */
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
        .ria_regs_rx_taken(rp6502_rx_taken)
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
