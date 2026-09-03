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
 * behind the address, the sram discipline.
 *
 * The clock is the ungated one. When the machine is stopped its clock
 * is taken away, but the array is not part of the machine's logic --
 * its addresses and its write enable come from logic that is stopped,
 * so they stand still and it does nothing. That is what lets the
 * savestate serializer borrow a port: it takes the render's side, which
 * is going nowhere, rather than asking a block RAM for a third.
 */

module xram (
    input logic clk,

    input logic [13:0] a_addr,
    output logic [31:0] xram_a_rdata,

    input logic [15:0] b_addr,
    input logic [7:0] b_wdata,
    input logic b_we,
    output logic [7:0] xram_b_rdata,

    /* The serializer, which owns both ports while it has the machine:
     * a word out of the render's side and a word in through the byte
     * side, all four lanes at once. */
    input logic sst_own,
    input logic [13:0] sst_addr,
    input logic sst_we,
    input logic [31:0] sst_wdata
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

    logic [13:0] a_a;
    always_comb a_a = sst_own ? sst_addr : a_addr;
    always_ff @(posedge clk) begin
        xram_a_rdata <= {mem3[a_a], mem2[a_a], mem1[a_a], mem0[a_a]};
    end

    logic [13:0] b_word;
    logic [1:0] b_sel;
    logic w0, w1, w2, w3;
    always_comb begin
        b_word = sst_own ? sst_addr : b_addr[15:2];
        b_sel = b_addr[1:0];
        w0 = sst_own ? sst_we : (b_we && b_sel == 2'd0);
        w1 = sst_own ? sst_we : (b_we && b_sel == 2'd1);
        w2 = sst_own ? sst_we : (b_we && b_sel == 2'd2);
        w3 = sst_own ? sst_we : (b_we && b_sel == 2'd3);
    end

    logic [31:0] b_q;
    logic [1:0] b_lane;
    always_ff @(posedge clk) begin
        if (w0) mem0[b_word] <= sst_own ? sst_wdata[7:0] : b_wdata;
        if (w1) mem1[b_word] <= sst_own ? sst_wdata[15:8] : b_wdata;
        if (w2) mem2[b_word] <= sst_own ? sst_wdata[23:16] : b_wdata;
        if (w3) mem3[b_word] <= sst_own ? sst_wdata[31:24] : b_wdata;
        b_q <= {mem3[b_word], mem2[b_word], mem1[b_word], mem0[b_word]};
        b_lane <= b_sel;
    end
    always_comb xram_b_rdata = b_q[8*b_lane+:8];

endmodule
