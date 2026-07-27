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
 *   0x20000000  the RIA register cells; +0x40 pops the 6502's TX ring,
 *               +0x100 the xstack bytes, +0x320 the xstack pointer,
 *               +0x48 offers an RX byte toward the $FFE2 latch
 *   0x40000000  control: bit 0 runs the 6502 (its RESB, inverted)
 *   0x40000004  syscall: bit 0 reads pending; any write acknowledges
 *   0x50000000  the terminal cell memory and scanout registers, word-wide
 *   0x60000000  staging, read-only: the platform answers with the byte at
 *               rp6502_stage_addr — the APF data slot on the Pocket, the
 *               bridge model in simulation
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

    /* Staging window: address out at the strobe, byte answered by the
     * platform before the next system clock. */
    output logic [27:0] rp6502_stage_addr,
    input logic [7:0] stage_rdata,

    /* The composed picture, aligned with its data enable. */
    output logic [15:0] rp6502_vid_pixel,
    output logic rp6502_vid_de,

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
        .irq_i(via_irq || ria_irq),
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

    logic ria_irq;
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

    /* Byte lane per sb/lb for the byte-wide windows. */
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
        bus_sel_vid;
    always_comb begin
        bus_sel_sram = bus_addr[31:28] == 4'h1;
        bus_sel_regs = bus_addr[31:28] == 4'h2;
        bus_sel_ctl = bus_addr[31:28] == 4'h4;
        bus_sel_stage = bus_addr[31:28] == 4'h6;
        bus_sel_vid = bus_addr[31:28] == 4'h5;
    end

    logic api_pending;
    logic bus_ctl_api;
    logic [7:0] sram_b_rdata;
    logic [31:0] regs_b_rdata, regs_b_q;
    logic [31:0] vid_b_rdata;
    // Which target answers: 0 sram, 1 regs, 2 control, 3 staging, 4 vid.
    logic [2:0] bus_rsel;
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n) begin
            cpu_run <= 1'b0;
            bus_rsel <= 3'd0;
            bus_ctl_api <= 1'b0;
            rp6502_stage_addr <= '0;
        end else begin
            if (bus_stb) begin
                bus_rsel <= bus_sel_regs ? 3'd1
                    : (bus_sel_ctl ? 3'd2
                    : (bus_sel_stage ? 3'd3
                    : (bus_sel_vid ? 3'd4 : 3'd0)));
                bus_ctl_api <= bus_addr[2];
                rp6502_stage_addr <= bus_addr[27:0];
                /* Captured at the strobe: ring reads advance their pointer
                 * there, so the answer must not be re-derived afterward. */
                regs_b_q <= regs_b_rdata;
            end
            if (bus_stb && bus_we && bus_sel_ctl && !bus_addr[2])
                cpu_run <= bus_wbyte[0];
        end
    end

    /* Reads answer one cycle after the strobe. The register window is a
     * true word; the byte-wide windows put their byte on every lane so the
     * master's own extract picks the addressed one. */
    logic [7:0] bus_rbyte;
    always_comb begin
        case (bus_rsel)
            3'd2: bus_rbyte = bus_ctl_api ? {7'b0, api_pending}
                : {7'b0, cpu_run};
            3'd3: bus_rbyte = stage_rdata;
            default: bus_rbyte = sram_b_rdata;
        endcase
        bus_rdata = bus_rsel == 3'd1 ? regs_b_q
            : (bus_rsel == 3'd4 ? vid_b_rdata : {4{bus_rbyte}});
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
        .vsync_pulse(vid_vsync_pulse),
        .ria_regs_irq(ria_irq),
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

    /* The raster: one clock per pixel in simulation, the pixel domain on
     * hardware. The scanline port is the beam's line. */
    logic [9:0] vid_h /*verilator public_flat_rd*/;
    logic [9:0] vid_v /*verilator public_flat_rd*/;
    logic vid_de /*verilator public_flat_rd*/;
    logic vid_hsync /*verilator public_flat_rd*/;
    logic vid_vsync /*verilator public_flat_rd*/;
    logic vid_line_start, vid_frame_start;
    logic vid_vsync_pulse;
    vid_timing vid_timing (
        .clk(clk_sys),
        .rst_n(rst_n),
        .vid_timing_h(vid_h),
        .vid_timing_v(vid_v),
        .vid_timing_de(vid_de),
        .vid_timing_hsync(vid_hsync),
        .vid_timing_vsync(vid_vsync),
        .vid_timing_line_start(vid_line_start),
        .vid_timing_frame_start(vid_frame_start),
        .vid_timing_vsync_pulse(vid_vsync_pulse)
    );
    always_comb rp6502_scanline = vid_v;

    logic [15:0] term_pix;
    vid_term vid_term (
        .clk(clk_sys),
        .rst_n(rst_n),
        .frame_start(vid_frame_start),
        .h(vid_h),
        .v(vid_v),
        .line_start(vid_line_start),
        .vid_term_pix(term_pix),
        .b_stb(bus_stb && bus_sel_vid),
        .b_we(bus_we),
        .b_addr(bus_addr[16:0]),
        .b_wstrb(bus_wstrb),
        .b_wdata(bus_wdata),
        .vid_term_b_rdata(vid_b_rdata)
    );

    vid_compose vid_compose (
        .clk(clk_sys),
        .rst_n(rst_n),
        .de(vid_de),
        .plane0(term_pix),
        .vid_compose_pix(rp6502_vid_pixel),
        .vid_compose_de(rp6502_vid_de)
    );

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid;
    always_comb unused_vid = ^{vid_hsync, vid_vsync};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
