/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502's 64 KB, one true-dual-port BRAM. Port A belongs to the CPU; port
 * B is the soft CPU's, for program loading and the OS's ram reads. Reads are
 * synchronous, one clock behind the address — the machine runs PHI2 at least
 * two system clocks wide, so data is ready well before the CPU samples.
 *
 * The clock is the ungated one. A stopped machine's logic holds both
 * addresses and both write enables still, so the array does nothing;
 * the savestate serializer borrows port B, which the soft CPU is not
 * using because the soft CPU is stopped too.
 */

module sram (
    input logic clk,

    input logic [15:0] a_addr,
    input logic [7:0] a_wdata,
    input logic a_we,
    output logic [7:0] sram_a_rdata,

    input logic [15:0] b_addr,
    input logic [7:0] b_wdata,
    input logic b_we,
    output logic [7:0] sram_b_rdata,

    input logic sst_own,
    input logic [15:0] sst_addr,
    input logic sst_we,
    input logic [7:0] sst_wdata
);

    /* The mixed-port collision is undefined here as it is on the
     * real dual-core part; without this the fitter builds the whole
     * 64 KB out of registers. */
    (* ramstyle = "no_rw_check" *)
    logic [7:0] mem[65536] /*verilator public_flat_rw*/;

    /* The !sst_own: port A's write enable is machine logic, and when
     * the machine's clock is cut it freezes wherever it was. Frozen
     * high, it would re-write its stale byte here -- on a clock that
     * did not stop -- every cycle of the savestate. */
    always_ff @(posedge clk) begin
        if (a_we && !sst_own)
            mem[a_addr] <= a_wdata;
        sram_a_rdata <= mem[a_addr];
    end

    logic [15:0] b_a;
    logic b_w;
    logic [7:0] b_d;
    always_comb begin
        b_a = sst_own ? sst_addr : b_addr;
        b_w = sst_own ? sst_we : b_we;
        b_d = sst_own ? sst_wdata : b_wdata;
    end
    always_ff @(posedge clk) begin
        if (b_w)
            mem[b_a] <= b_d;
        sram_b_rdata <= mem[b_a];
    end

endmodule
