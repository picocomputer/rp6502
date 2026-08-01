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

    /* One array per byte lane. The byte side writes a lane at a time,
     * and a dynamic part-select into a wide word is something no block
     * RAM can be built from — the lanes make each write whole. */
    (* ramstyle = "no_rw_check" *)
    logic [7:0] mem0[16384] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] mem1[16384] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] mem2[16384] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] mem3[16384] /*verilator public_flat_rw*/;

    always_ff @(posedge clk) begin
        xram64k_a_rdata <= {mem3[a_addr], mem2[a_addr],
                            mem1[a_addr], mem0[a_addr]};
    end

    logic [13:0] b_word;
    always_comb b_word = b_addr[15:2];

    logic [31:0] b_q;
    logic [1:0] b_lane;
    always_ff @(posedge clk) begin
        if (b_we && b_addr[1:0] == 2'd0)
            mem0[b_word] <= b_wdata;
        if (b_we && b_addr[1:0] == 2'd1)
            mem1[b_word] <= b_wdata;
        if (b_we && b_addr[1:0] == 2'd2)
            mem2[b_word] <= b_wdata;
        if (b_we && b_addr[1:0] == 2'd3)
            mem3[b_word] <= b_wdata;
        b_q <= {mem3[b_word], mem2[b_word], mem1[b_word], mem0[b_word]};
        b_lane <= b_addr[1:0];
    end
    always_comb xram64k_b_rdata = b_q[8*b_lane+:8];

endmodule
