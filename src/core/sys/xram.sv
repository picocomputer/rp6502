/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 64 KB XRAM, one true-dual-port BRAM organized as words. Port A is
 * the render side's, read-only, and it serves two readers, the fill and
 * the sprite stage, a word each every machine clock: its clock runs at
 * twice the machine's, and each reader owns one of the two edges, so
 * neither ever waits for the other. Port B is the system side's byte
 * lane: the RW engine and the soft CPU behind their arbiter, and the
 * savestate serializer, which reads and writes whole words through it.
 *
 * The array's clocks are the ungated ones, and keep running while the
 * machine is stopped for a savestate. Port B's address and write enable
 * come from logic that is stopped, so they stand still and it does
 * nothing. Port A's addresses are registered on the machine's own clock
 * and its words handed over once for each of the machine's edges, so a
 * read in flight when the machine stops keeps its address and its word,
 * and the word that lands after the stop is the one it asked for.
 */

module xram (
    /* The system clock, port B's, and the machine's, which a savestate
     * stops. */
    input logic clk,
    input logic clk_mach,

    /* Port A's clock is the machine's doubled and shifted by 9.0 ns, so
     * its edges land 9.0 and 18.9 ns after each machine edge, both clear
     * of it: the registered addresses have 9.0 ns to reach the block, and
     * the words 10.9 ns to reach the machine's registers from the edge
     * they come out on, which is where the margins balance. clk_ph is a machine-rate clock
     * shifted to be low across the first of those edges and high across
     * the second; it is taken as data, and tells the two apart. */
    input logic clk_a2,
    input logic clk_ph,
    input logic [13:0] f_addr,
    output logic [31:0] xram_f_rdata,
    input logic [13:0] s_addr,
    output logic [31:0] xram_s_rdata,

    input logic [15:0] b_addr,
    input logic [7:0] b_wdata,
    input logic b_we,
    output logic [7:0] xram_b_rdata,

    input logic sst_own,
    input logic [13:0] sst_addr,
    input logic sst_we,
    input logic [31:0] sst_wdata,
    output logic [31:0] xram_sst_rdata
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

    /* Port A reads on every edge, the sprite stage's address on the
     * first after the machine's and the fill's on the second. Both are
     * registered on the machine's edge first, because the arithmetic
     * that makes each of them fills a machine clock by itself. Each
     * word is kept in this clock's registers until the first edge of
     * the next machine clock, so the machine reads both on the edge
     * after that and never on one where they change: a reader has its
     * word two clocks after it presents an address. */
    logic ph;
    logic [13:0] f_addr_q, s_addr_q, a_a;
    logic [31:0] a_q, s_raw;
    always_comb a_a = ph ? s_addr_q : f_addr_q;
    /* m_tog flips on each of the machine's edges and m_seen follows it
     * a handover later, so a handover with no machine edge before it
     * leaves the words alone. */
    logic m_tog, m_seen;
    initial begin
        m_tog = 1'b0;
        m_seen = 1'b0;
        ph = 1'b0;
        f_addr_q = '0;
        s_addr_q = '0;
        a_q = '0;
        s_raw = '0;
        xram_f_rdata = '0;
        xram_s_rdata = '0;
    end
    always_ff @(posedge clk_mach) begin
        f_addr_q <= f_addr;
        s_addr_q <= s_addr;
        m_tog <= !m_tog;
    end
    always_ff @(posedge clk_a2) begin
        ph <= clk_ph;
        a_q <= {mem3[a_a], mem2[a_a], mem1[a_a], mem0[a_a]};
        if (ph) begin
            if (m_seen != m_tog) begin
                xram_f_rdata <= a_q;
                xram_s_rdata <= s_raw;
            end
            m_seen <= m_tog;
        end else
            s_raw <= a_q;
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
    always_comb xram_sst_rdata = b_q;

endmodule
