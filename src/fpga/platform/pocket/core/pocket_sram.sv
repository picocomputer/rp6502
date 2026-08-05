/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502's 64 KB, on the Pocket's own SRAM chip instead of block
 * memory. AS6C2016-55BIN: 128K x 16, asynchronous, 55 ns, and it has
 * been tied off dead in core_top.sv since this port began.
 *
 * It is worth 64 M10K blocks, which is exactly what doubling the soft
 * CPU's TCM costs, and it is a better fit for this job than the SDRAM
 * could ever be. The 6502 gets 119 ns per access at 8 MHz — six clocks
 * at the accumulator's tightest — and this part answers in 55 ns every
 * time. No rows, no refresh, no banks, no pattern that defeats it.
 * The SDRAM is fast on average and slow when it is unlucky, which is
 * the wrong shape for a machine whose contract is that the clock you
 * asked for is the clock you get.
 *
 * BYTE WIDE, NOT HALFWORD. The chip is 16 bits and only the low lane is
 * used: A0-A15 carry the 6502's address directly, A16 is tied low, LB#
 * is always asserted and UB# never is. Reading halfwords and holding
 * the odd byte — which is what the SDRAM controller does — buys nothing
 * here, because there is no per-access overhead to amortise. tAA is
 * 55 ns for one byte or two, so the byte you saved costs the same as
 * the byte you fetched. It would have bought a couple of milliamps and
 * cost a hold register, a comparator and two invalidation paths.
 *
 * NO CHIP ENABLE. Analogue's port list has none, so the board ties it
 * low and this part can never be deselected. WE# is therefore the only
 * interlock against a write, and it is registered with a known power-on
 * state for that reason: a glitch on it is a write to whatever address
 * happens to be standing, and nothing downstream would ever know.
 *
 * ONE PORT, TWO MASTERS. Block memory gave the soft CPU a second port
 * for free and this does not. The 6502 owns the chip while it runs; the
 * soft CPU's window is served when it is halted, which is when the only
 * two users of that window exist — the loader writing an image, and the
 * boot program's read-back. If it is asked for while the 6502 runs, the
 * machine is held for a cycle rather than deadlocked.
 */

module pocket_sram (
    input logic clk,

    /* The 6502's advance, before this module's own hold is applied. */
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
    /* Suppresses the next phi2_en so a soft CPU access cannot land on
     * top of the 6502's. */
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

    /* Four clocks, 79.4 ns, against tAA's 55. The parts are a launch
     * register into the pad, the chip's own access, and the pad back
     * into a capture register; the board contributes almost nothing
     * because the chip sits as close to the die as it can be put.
     *
     * Three clocks is arithmetically impossible — 59.5 ns of budget
     * against 55 ns of tAA leaves nothing for either pad crossing — and
     * five does not fit the 6502, which samples on the sixth.
     *
     * The address comes from cpu65's pre-registration bus, so the launch
     * happens ON the enable rather than a clock after it. Taking the
     * registered address instead cost a whole clock at both ends: it
     * fetched the previous cycle's byte, and once that was fixed it left
     * the byte arriving on the fifth clock with only the sixth to reach
     * the CPU's address cone — which the fitter measured 0.604 ns short
     * of. Launching on the enable gives the chip four clocks and the
     * cone two. */
    localparam int PH_LAST = 3;

    /* Both ports run the same four clocks, because the chip is the same
     * chip either way. Port B was twice as long for a while, which was
     * never about the SRAM: the address register is shared, the 6502's
     * path into it is a multicycle that the SDC grants four clocks, and
     * a shared register takes the loosest constraint written against it,
     * so the soft CPU's ordinary logic inherited a grant it had not
     * earned. Doubling its access hid that. The SDC now says the true
     * thing about each path separately, so there is nothing left to hide
     * from and the port is as long as tAA needs. */
    logic [1:0] ph;
    logic busy_a, busy_b, b_done;
    logic serving_we;

    initial begin
        ph = '0;
        busy_a = 1'b0;
        busy_b = 1'b0;
        b_done = 1'b0;
        serving_we = 1'b0;
        pocket_sram_a = '0;
        pocket_sram_dq_out = '0;
        pocket_sram_dq_oe = 1'b0;
        pocket_sram_oe_n = 1'b1;
        /* The one that matters at power-on: the board holds CE# low, so
         * a WE# that came up asserted would be writing already. */
        pocket_sram_we_n = 1'b1;
        pocket_sram_ub_n = 1'b1;
        pocket_sram_lb_n = 1'b1;
        pocket_sram_a_rdata = '0;
        pocket_sram_b_rdata = '0;
    end

    /* The stall lifts on a registered done rather than on the terminal
     * phase itself, which is what makes the byte reliably older than the
     * strobe that collects it. Lifting combinationally in the same clock
     * that captured the byte handed back the PREVIOUS access's data —
     * every soft CPU read one behind, which a bench ladder caught by
     * writing A5 then 3C and reading back 3C twice.
     *
     * Nothing waits on this port: it serves the loader and the boot
     * read-back, both with the 6502 halted. A spare clock is free here
     * and an off-by-one is not. */
    always_comb begin
        pocket_sram_b_stall = b_stb && !b_done;
        pocket_sram_hold = busy_b || (b_stb && !busy_a);
    end

    always_ff @(posedge clk) begin
        /* The done flag lives until the soft CPU takes its answer away. */
        if (!b_stb)
            b_done <= 1'b0;

        /* Defaults: the bus released, no write, the part deselected by
         * its byte enables. Deselecting with LB#/UB# rather than OE# is
         * what the datasheet's second standby row allows, and with CE#
         * strapped low it is the only standby there is. */
        pocket_sram_dq_oe <= 1'b0;
        pocket_sram_oe_n <= 1'b1;
        pocket_sram_we_n <= 1'b1;

        if (busy_a || busy_b) begin
            if (ph == 2'(PH_LAST)) begin
                if (busy_a)
                    pocket_sram_a_rdata <= sram_dq_in[7:0];
                else begin
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
                /* tWP wants 45 ns; three clocks is 59.5. */
                pocket_sram_we_n <= !(serving_we && ph < 2'(PH_LAST));
            end
        end else if (phi2_en && cpu_run && !pocket_sram_hold) begin
            busy_a <= 1'b1;
            ph <= 2'd0;
            serving_we <= a_we;
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
