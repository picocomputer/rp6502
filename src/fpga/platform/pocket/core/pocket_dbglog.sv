/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console again, this time out through the Pocket itself. The debug
 * pin is on the 6515D breakout board; this needs nothing but a Pocket
 * with debug logging switched on, which is the difference between a log
 * anyone can read and a log that needs hardware to read.
 *
 * Target command 0x0152 carries one 32-bit event id, so four console
 * bytes ride in each one, first byte in the top eight bits — the hex the
 * log prints then reads left to right as the text. A partial word is
 * flushed on its own after a short quiet period, because the last line
 * before a hang is the one worth having and it is exactly the one that
 * would otherwise sit in the packer.
 *
 * One event is a round trip through the host, so this is slow, and the
 * queue in front of it is a kilobyte for that reason. It still drops
 * when the host falls far enough behind; a log that stalls the machine
 * it is logging would report on a machine that no longer exists.
 */

module pocket_dbglog #(
    /* About 0.9 ms of quiet at 74.25 MHz before a short word goes. */
    parameter int FLUSH_TICKS = 65536
) (
    input logic clk_sys,
    input logic rst_n,
    input logic [7:0] tx_data,
    input logic tx_valid,
    input logic [7:0] rv_tx_data,
    input logic rv_tx_valid,

    input logic clk_74a,
    input logic arst_n,
    input logic target_debug_done,
    output logic pocket_dbglog_event,
    output logic [31:0] pocket_dbglog_id
);

    logic [7:0] byte_in;
    always_comb byte_in = rv_tx_valid ? rv_tx_data : tx_data;

    logic fifo_full, fifo_empty, take;
    logic [7:0] byte_out;

    pocket_fifo #(
        .WIDTH(8),
        .DEPTH_LOG2(10)
    ) q (
        .wclk(clk_sys),
        .wrst_n(rst_n),
        .w_stb(tx_valid || rv_tx_valid),
        .w_data(byte_in),
        .pocket_fifo_full(fifo_full),
        .rclk(clk_74a),
        .rrst_n(arst_n),
        .r_take(take),
        .pocket_fifo_empty(fifo_empty),
        .pocket_fifo_rdata(byte_out)
    );

    /* The bridge holds its done high from one command until the next is
     * issued, so an edge is the only thing worth believing: arm, watch
     * it fall, then watch it rise. */
    localparam logic [1:0] S_FILL = 2'd0;
    localparam logic [1:0] S_ARM = 2'd1;
    localparam logic [1:0] S_WAIT = 2'd2;

    logic [1:0] state;
    logic [1:0] count;
    logic [$clog2(FLUSH_TICKS)-1:0] quiet;
    /* About 226 ms at 74.25 MHz before an unanswered event is dropped. */
    logic [23:0] stall;

    always_comb take = !fifo_empty && state == S_FILL;

    /* Two's complement in two bits is 4 - count for the one, two and
     * three byte cases, which is the shift that left-justifies them. */
    logic [1:0] pad;
    always_comb pad = 2'd0 - count;

    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            pocket_dbglog_event <= 1'b0;
            pocket_dbglog_id <= '0;
            state <= S_FILL;
            count <= '0;
            quiet <= '0;
            stall <= '0;
        end else if (state != S_FILL && &stall) begin
            /* The host answers target commands only once it is minded
             * to. An event posted before then must not take the log
             * down with it — the interesting bytes come later. */
            pocket_dbglog_event <= 1'b0;
            state <= S_FILL;
        end else
            case (state)
                S_ARM: begin
                    stall <= stall + 1'b1;
                    if (!target_debug_done) state <= S_WAIT;
                end
                S_WAIT:
                if (target_debug_done) begin
                    pocket_dbglog_event <= 1'b0;
                    state <= S_FILL;
                end else stall <= stall + 1'b1;
                default:
                if (take) begin
                    pocket_dbglog_id <= {pocket_dbglog_id[23:0], byte_out};
                    quiet <= '0;
                    if (count == 2'd3) begin
                        count <= '0;
                        pocket_dbglog_event <= 1'b1;
                        stall <= '0;
                        state <= S_ARM;
                    end else count <= count + 2'd1;
                end else if (count != 2'd0) begin
                    if (quiet == ($clog2(FLUSH_TICKS))'(FLUSH_TICKS - 1)) begin
                        pocket_dbglog_id <= pocket_dbglog_id << {pad, 3'b000};
                        count <= '0;
                        quiet <= '0;
                        pocket_dbglog_event <= 1'b1;
                        stall <= '0;
                        state <= S_ARM;
                    end else quiet <= quiet + 1'b1;
                end
            endcase
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_dbglog;
    always_comb unused_pocket_dbglog = fifo_full;
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
