/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The Pocket's SRAM is an AS6C2016-55BIN, a 128K x 16 asynchronous part
 * with an address access time, tAA, of 55 ns. Analogue's port list has no
 * chip enable for it, so the part is always selected, only the byte
 * enables can deselect it, and WE# low while LB# or UB# is low is always
 * a write.
 */

module pocket_sram (
    input logic clk,
    /* The machine's clock, clk through the savestate's gate. The PHI2
     * enable is registered in machine logic, which freezes when that
     * clock stops while clk keeps running, so an enable frozen high would
     * launch port A again on every clk of the stop. Port A therefore
     * serves one access an enable: the first clk that sees the enable
     * launches it, which is the machine's own edge unless the machine
     * stopped just before it, and nothing launches again until the
     * machine has taken an edge. An access launched with no machine edge
     * at or after its clk holds its byte back until there is one, so the
     * byte the 6502 takes on its next edge is still the one for its
     * present address. */
    input logic clk_mach,
    input logic refill,
    input logic phi2_en,
    input logic cpu_run,

    input logic [15:0] a_addr,
    input logic [7:0] a_wdata,
    input logic a_we,
    output logic [7:0] pocket_sram_a_rdata,

    input logic [15:0] b_addr,
    input logic [7:0] b_wdata,
    input logic b_we,
    input logic b_stb,
    output logic [7:0] pocket_sram_b_rdata,
    output logic pocket_sram_b_stall,
    output logic pocket_sram_hold,

    output logic [16:0] pocket_sram_a,
    output logic [15:0] pocket_sram_dq_out,
    output logic pocket_sram_dq_oe,
    input logic [15:0] sram_dq_in,
    output logic pocket_sram_oe_n,
    output logic pocket_sram_we_n,
    output logic pocket_sram_ub_n,
    output logic pocket_sram_lb_n
);

    /* An access takes four clocks, 79.4 ns, against a tAA of 55 ns, and
     * the 24.4 ns left over covers the pad crossings out and back. Three
     * clocks would be 59.5 ns and leave 4.5 ns for both crossings.
     *
     * Except while refill is high during a savestate restore, port A's
     * address is cpu_next_addr, the value cpu_addr takes at the PHI2
     * enable edge, so the access launches on that edge. The byte is then
     * captured on the fourth clock and read by the 6502 on the sixth at
     * the earliest, which gives that path the two clocks that wiring.sdc
     * grants it. */
    localparam int PH_LAST = 3;

    logic [1:0] ph;
    logic busy_a, busy_b, b_done;
    logic serving_we, serving_refill;

    /* m_edge: the machine's clock rose on the last clk. */
    logic m_tog, m_q, m_edge;
    initial m_tog = 1'b0;
    always_ff @(posedge clk_mach) m_tog <= !m_tog;
    always_comb m_edge = m_tog != m_q;
    /* armed: the machine has taken an edge since port A last launched.
     * a_seen: it has taken one at or after this access's launch. a_pend:
     * a_next holds a byte waiting for that edge. */
    logic armed, a_seen, a_pend;
    logic [7:0] a_next;

    initial begin
        m_q = 1'b0;
        armed = 1'b1;
        a_seen = 1'b0;
        a_pend = 1'b0;
        a_next = '0;
        serving_refill = 1'b0;
        ph = '0;
        busy_a = 1'b0;
        busy_b = 1'b0;
        b_done = 1'b0;
        serving_we = 1'b0;
        pocket_sram_a = '0;
        pocket_sram_dq_out = '0;
        pocket_sram_dq_oe = 1'b0;
        pocket_sram_oe_n = 1'b1;
        pocket_sram_we_n = 1'b1;
        pocket_sram_ub_n = 1'b1;
        pocket_sram_lb_n = 1'b1;
        pocket_sram_a_rdata = '0;
        pocket_sram_b_rdata = '0;
    end

    always_comb begin
        /* pocket_sram_b_stall ends when b_done is set, on the same edge
         * that loads pocket_sram_b_rdata, so a port B requester samples
         * pocket_sram_b_rdata no earlier than the edge after that one. If
         * the stall ended while ph was PH_LAST, the requester would
         * sample pocket_sram_b_rdata on the edge that loads it and take
         * the previous access's byte. */
        pocket_sram_b_stall = b_stb && !b_done;
        /* pocket_sram_hold includes busy_a, so no PHI2 enable is taken
         * while port A still serves the last one or a refill. busy_a
         * never suppresses an enable during normal running, because PHI2
         * enables are at least six clocks apart and busy_a lasts four,
         * and a restore's refill ends while the 6502 is still held in
         * reset, thousands of clocks before it runs. */
        pocket_sram_hold = busy_a || busy_b || (b_stb && !busy_a);
    end

    always_ff @(posedge clk) begin
        if (!b_stb)
            b_done <= 1'b0;

        m_q <= m_tog;
        if (m_edge)
            armed <= 1'b1;
        if (m_edge)
            a_seen <= 1'b1;
        if (a_pend && m_edge) begin
            pocket_sram_a_rdata <= a_next;
            a_pend <= 1'b0;
        end

        pocket_sram_dq_oe <= 1'b0;
        pocket_sram_oe_n <= 1'b1;
        pocket_sram_we_n <= 1'b1;

        if (busy_a || busy_b) begin
            if (ph == 2'(PH_LAST)) begin
                if (busy_a && (a_seen || m_edge || serving_refill)) begin
                    pocket_sram_a_rdata <= sram_dq_in[7:0];
                    a_pend <= 1'b0;
                end else if (busy_a) begin
                    a_next <= sram_dq_in[7:0];
                    a_pend <= 1'b1;
                end else begin
                    pocket_sram_b_rdata <= sram_dq_in[7:0];
                    b_done <= 1'b1;
                end
                busy_a <= 1'b0;
                busy_b <= 1'b0;
                pocket_sram_lb_n <= 1'b1;
                pocket_sram_ub_n <= 1'b1;
            end else begin
                ph <= ph + 2'd1;
                pocket_sram_dq_oe <= serving_we;
                pocket_sram_oe_n <= serving_we;
                pocket_sram_we_n <= !(serving_we && ph < 2'(PH_LAST));
            end
        end else if ((refill || (phi2_en && cpu_run && armed))
                     && !pocket_sram_hold) begin
            busy_a <= 1'b1;
            ph <= 2'd0;
            serving_we <= a_we;
            serving_refill <= refill;
            armed <= 1'b0;
            a_seen <= 1'b0;
            pocket_sram_a <= {1'b0, a_addr};
            pocket_sram_dq_out <= {8'h00, a_wdata};
            pocket_sram_lb_n <= 1'b0;
            pocket_sram_ub_n <= 1'b1;
        end else if (b_stb && !b_done) begin
            busy_b <= 1'b1;
            ph <= 2'd0;
            serving_we <= b_we;
            pocket_sram_a <= {1'b0, b_addr};
            pocket_sram_dq_out <= {8'h00, b_wdata};
            pocket_sram_lb_n <= 1'b0;
            pocket_sram_ub_n <= 1'b1;
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_sram;
    always_comb unused_pocket_sram = ^sram_dq_in[15:8];
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
