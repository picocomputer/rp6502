/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The Pocket's asynchronous SRAM for the bench, AS6C2016-55BIN:
 * 128K x 16, no clock, 55 ns from address to data. There is no chip
 * enable on this board — Analogue's port list has none, so it is
 * strapped low — and the byte enables are the whole of the deselect.
 *
 * THIS CHIP IS OURS, NOT THE BOARD'S. The part number and the datasheet
 * are real, but what this model does between the numbers is a reading
 * of them, and readings have been wrong here before: the SDRAM model
 * spent months demanding auto-precharge on every access because that is
 * what our own controller happened to do.
 *
 * Zero-delay, which is the important caveat. A bench cannot see a
 * setup violation, so passing here says the protocol is right and says
 * nothing at all about the 55 ns. What it does check is the thing a
 * zero-delay model can: that a write is only ever the deliberate
 * overlap of WE# and a byte enable, because with CE# strapped low that
 * overlap is the only interlock there is and a glitch on it corrupts
 * memory silently.
 */

module asram_model (
    input logic [16:0] a,
    inout wire [15:0] dq,
    input logic oe_n,
    input logic we_n,
    input logic ub_n,
    input logic lb_n
);

    logic [15:0] mem[1 << 17] /*verilator public_flat_rw*/;

    /* The part drives only while output is enabled, a byte is selected,
     * and no write is in progress. */
    logic drive_lo, drive_hi;
    always_comb begin
        drive_lo = !oe_n && we_n && !lb_n;
        drive_hi = !oe_n && we_n && !ub_n;
    end
    assign dq[7:0] = drive_lo ? mem[a][7:0] : 8'bz;
    assign dq[15:8] = drive_hi ? mem[a][15:8] : 8'bz;

    /* Writes land on the level, the way a static RAM does — this is a
     * latch on purpose, and it is the part's whole hazard: with CE#
     * strapped low, WE# and a byte enable being low together is the
     * only thing standing between the address bus and the array. */
    always_latch begin
        if (!we_n && !lb_n)
            mem[a][7:0] = dq[7:0];
        if (!we_n && !ub_n)
            mem[a][15:8] = dq[15:8];
    end

endmodule
