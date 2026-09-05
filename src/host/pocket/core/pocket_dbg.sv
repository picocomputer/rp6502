/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's console port, out the debug pin: the firmware's own
 * printf and the 6502's $FFE1 writes as the firmware forwards them, one
 * copy in the order the firmware saw them, which is exactly what the
 * simulation prints and what made it debuggable.
 *
 * It runs on the host's clock, not the machine's. A debug channel that
 * needs the PLL is no use when the PLL is what is broken — this way the
 * pin still talks even if everything downstream of the PLL is dead, and
 * silence means something specific rather than being ambiguous. The
 * bytes cross from the machine on the same queue the bridge uses. A
 * dropped byte here costs a character of a log; there is nothing to be
 * gained by making it cost more.
 */

module pocket_dbg #(
    /* 74.25 MHz to 115200 baud. */
    parameter int DIVISOR = 644
) (
    /* The machine's clock, which the savestate gate stops. The bytes
     * are the machine's and their valid is a level it drives: on the
     * clock behind the gate, a valid frozen high by a stop would push
     * the same byte on every edge for the whole savestate. */
    input logic clk_mach,
    input logic rst_n,
    input logic [7:0] rv_tx_data,
    input logic rv_tx_valid,

    input logic clk_74a,
    input logic arst_n,
    output logic pocket_dbg_tx
);

    logic fifo_full, fifo_empty, take;
    logic [7:0] byte_out;

    pocket_fifo #(
        .WIDTH(8),
        .DEPTH_LOG2(6)
    ) q (
        .wclk(clk_mach),
        .w_stb(rv_tx_valid),
        .w_data(rv_tx_data),
        .pocket_fifo_full(fifo_full),
        .rclk(clk_74a),
        .r_take(take),
        .pocket_fifo_empty(fifo_empty),
        .pocket_fifo_rdata(byte_out)
    );

    /* Eight bits, one stop, no parity, idling high. */
    logic [9:0] shifter;
    logic [3:0] bits_left;
    logic [15:0] baud;

    always_comb take = !fifo_empty && bits_left == 4'd0 && baud == 16'd0;

    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            shifter <= 10'h3FF;
            bits_left <= '0;
            baud <= '0;
            pocket_dbg_tx <= 1'b1;
        end else if (baud != 16'd0) begin
            baud <= baud - 16'd1;
        end else if (bits_left != 4'd0) begin
            pocket_dbg_tx <= shifter[0];
            shifter <= {1'b1, shifter[9:1]};
            bits_left <= bits_left - 4'd1;
            baud <= 16'(DIVISOR - 1);
        end else if (!fifo_empty) begin
            /* start low, the byte, then the stop bit rides in the fill */
            shifter <= {1'b1, byte_out, 1'b0};
            bits_left <= 4'd10;
            baud <= '0;
        end else
            pocket_dbg_tx <= 1'b1;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_dbg;
    always_comb unused_pocket_dbg = fifo_full;
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
