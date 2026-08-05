/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502's 64 KB, one true-dual-port BRAM. Port A belongs to the CPU; port
 * B is the soft CPU's, for program loading and the OS's ram reads. Reads are
 * synchronous, one clock behind the address — the machine runs PHI2 at least
 * two system clocks wide, so data is ready well before the CPU samples.
 */

module sram64k (
    input logic clk,

    input logic [15:0] a_addr,
    input logic [7:0] a_wdata,
    input logic a_we,
    output logic [7:0] a_rdata,

    input logic [15:0] b_addr,
    input logic [7:0] b_wdata,
    input logic b_we,
    output logic [7:0] b_rdata
);

    /* The mixed-port collision is undefined here as it is on the
     * real dual-core part; without this the fitter builds the whole
     * 64 KB out of registers. */
    (* ramstyle = "no_rw_check" *)
    logic [7:0] mem[65536] /*verilator public_flat_rw*/;

    always_ff @(posedge clk) begin
        if (a_we)
            mem[a_addr] <= a_wdata;
        a_rdata <= mem[a_addr];
    end

    always_ff @(posedge clk) begin
        if (b_we)
            mem[b_addr] <= b_wdata;
        b_rdata <= mem[b_addr];
    end

endmodule
