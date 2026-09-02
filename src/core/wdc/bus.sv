/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502's bus: who answers an address, and what the wires hold when
 * nobody does.
 *
 * The 6502 is the only master, so this is the whole of it -- decode the
 * address, pick the byte, and remember the last one. bus.c is the same
 * concern in C, arranged the other way round: there each device tests its own
 * window and bus_tick only sequences them, because a software device that is
 * not selected can simply not run. Here every device is clocked regardless
 * and something has to choose which one the CPU hears.
 */

module bus (
    input logic clk,
    input logic en,

    /* The 6502's side. dout is what it drives on a write, which the bus
     * carries as surely as a byte it read. */
    input logic [15:0] addr,
    input logic we,
    input logic [7:0] dout,

    /* What each device is offering this cycle. The read ranges do not
     * overlap, so their order below is not load-bearing. */
    input logic [7:0] ria_data,
    input logic [7:0] via_data,
    input logic [7:0] sram_rdata,

    output logic [7:0] bus_din,
    output logic bus_sel_via,
    output logic bus_sel_ria
);

    /* Open bus carries whatever the bus last held, and on a write the 6502 is
     * what drives it. Gating this on reads would return the last byte read
     * instead, which the C emulation does not do and silicon does not either.
     *
     * Not reset by RESB, where bus.c's parked byte is. Nothing can see the
     * difference -- a reset ignores the data bus and every device re-drives
     * before the first fetch -- but the two implementations do differ here. */
    logic [7:0] hold;

    /* A4-A15 decoded off-chip into CS1 on the board (os.rst), and the RIA
     * takes the top 32 bytes. What is left in the page is mapped to nothing.
     * Which register inside a window is the device's own business, which is
     * why the low nibble goes to them and not here. */
    logic sel_open;
    always_comb begin
        bus_sel_via = addr[15:4] == 12'hFFD;
        bus_sel_ria = addr[15:5] == 11'b1111_1111_111;
        sel_open = addr[15:8] == 8'hFF && !bus_sel_via && !bus_sel_ria;
    end

    always_comb begin
        if (bus_sel_ria) bus_din = ria_data;
        else if (bus_sel_via) bus_din = via_data;
        else if (sel_open) bus_din = hold;
        else bus_din = sram_rdata;
    end

    initial hold = 8'h00;
    always_ff @(posedge clk) if (en) hold <= we ? dout : bus_din;

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_bus;
    always_comb unused_bus = ^{addr[3:0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
