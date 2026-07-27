/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The RP6502 machine, independent of the FPGA platform hosting it. Platform
 * wrappers under platform/ adapt this to the Analogue Pocket (APF) or MiSTer;
 * the simulation verilates this module directly and drives it from sim/.
 */

module rp6502
    import rp6502_pkg::*;
(
    input logic clk_sys,
    input logic rst_n,

    output logic [RP6502_SCANLINE_W-1:0] rp6502_scanline
);

    always_ff @(posedge clk_sys or negedge rst_n)
        if (!rst_n)
            rp6502_scanline <= '0;
        else if (rp6502_scanline == RP6502_SCANLINE_W'(RP6502_V_TOTAL - 1))
            rp6502_scanline <= '0;
        else
            rp6502_scanline <= rp6502_scanline + 1'b1;

endmodule
