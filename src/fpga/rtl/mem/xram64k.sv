/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 64 KB XRAM, one true-dual-port BRAM organized as words. Port A is
 * the render side's, read-only, one word per clock full-time — on hardware
 * it lives in the render domain, and the mixed pacing is why the array is
 * 32 bits wide. Port B is the system side's byte lane: the RW engine and
 * the soft CPU behind their arbiter. Reads are synchronous, one clock
 * behind the address, the sram64k discipline.
 */

module xram64k (
    input logic clk,

    input logic [13:0] a_addr,
    output logic [31:0] xram64k_a_rdata,

    input logic [15:0] b_addr,
    input logic [7:0] b_wdata,
    input logic b_we,
    output logic [7:0] xram64k_b_rdata
);

    logic [31:0] mem[16384] /*verilator public_flat_rw*/;

    always_ff @(posedge clk) begin
        xram64k_a_rdata <= mem[a_addr];
    end

    logic [31:0] b_q;
    logic [1:0] b_lane;
    always_ff @(posedge clk) begin
        if (b_we)
            mem[b_addr[15:2]][8*b_addr[1:0]+:8] <= b_wdata;
        b_q <= mem[b_addr[15:2]];
        b_lane <= b_addr[1:0];
    end
    always_comb xram64k_b_rdata = b_q[8*b_lane+:8];

endmodule
