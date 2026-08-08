/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A small asynchronous FIFO for the platform seams: gray-coded
 * pointers, two-flop crossings, show-ahead read. The flags err on the
 * safe side of the synchronizer lag — full may hold a beat after the
 * reader drains, empty a beat after the writer lands — and a push
 * honored under !full can never overflow, a take under !empty never
 * reads air.
 */

module pocket_fifo #(
    parameter int WIDTH = 32,
    parameter int DEPTH_LOG2 = 3
) (
    input logic wclk,
    input logic w_stb,
    input logic [WIDTH-1:0] w_data,
    output logic pocket_fifo_full,

    input logic rclk,
    input logic r_take,
    output logic pocket_fifo_empty,
    output logic [WIDTH-1:0] pocket_fifo_rdata
);

    localparam int PW = DEPTH_LOG2 + 1;

    /* These queues are four to sixteen words deep and the deepest is
     * forty-one bits wide, which does not fit a block RAM's forty-bit
     * mode. The read is asynchronous, which is what a LAB's memory does
     * natively and a block cannot do at all. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [WIDTH-1:0] mem[1 << DEPTH_LOG2];

    /* Preserved: two flops in series with nothing between them are
     * equivalent, and without a reset to tell them apart the fitter
     * merges them and the crossing loses its synchroniser. */
    logic [PW-1:0] wptr, wptr_gray, rptr, rptr_gray;
    (* preserve *) logic [PW-1:0] rptr_gray_w1, rptr_gray_w2;
    (* preserve *) logic [PW-1:0] wptr_gray_r1, wptr_gray_r2;

    logic [PW-1:0] wptr_n, rptr_n;
    always_comb wptr_n = wptr + PW'(w_stb && !pocket_fifo_full);
    always_comb rptr_n = rptr + PW'(r_take && !pocket_fifo_empty);

    initial begin
        wptr = '0;
        wptr_gray = '0;
        rptr_gray_w1 = '0;
        rptr_gray_w2 = '0;
    end
    always_ff @(posedge wclk) begin
        if (w_stb && !pocket_fifo_full)
            mem[wptr[DEPTH_LOG2-1:0]] <= w_data;
        wptr <= wptr_n;
        wptr_gray <= wptr_n ^ (wptr_n >> 1);
        rptr_gray_w1 <= rptr_gray;
        rptr_gray_w2 <= rptr_gray_w1;
    end

    initial begin
        rptr = '0;
        rptr_gray = '0;
        wptr_gray_r1 = '0;
        wptr_gray_r2 = '0;
    end
    always_ff @(posedge rclk) begin
        rptr <= rptr_n;
        rptr_gray <= rptr_n ^ (rptr_n >> 1);
        wptr_gray_r1 <= wptr_gray;
        wptr_gray_r2 <= wptr_gray_r1;
    end

    /* Full: the write pointer has lapped the (synchronized) read
     * pointer — gray codes differ only in the top two bits. */
    always_comb pocket_fifo_full =
        wptr_gray == {~rptr_gray_w2[PW-1:PW-2], rptr_gray_w2[PW-3:0]};
    always_comb pocket_fifo_empty = rptr_gray == wptr_gray_r2;

    always_comb pocket_fifo_rdata = mem[rptr[DEPTH_LOG2-1:0]];

endmodule
