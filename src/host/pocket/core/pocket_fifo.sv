/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The write side compares its pointer with the read pointer after two
 * synchronizer flops, and the read side does the same with the write
 * pointer, so the flags are pessimistic: full can stay set for a few
 * write clocks after a read, and empty for a few read clocks after a
 * write. A write accepted while full is clear therefore never
 * overflows, and a take while empty is clear never reads an entry that
 * has not been written.
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

    /* mem is read asynchronously, which an MLAB supports and an M10K
     * block does not. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [WIDTH-1:0] mem[1 << DEPTH_LOG2];

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

    /* The write pointer is one lap ahead of the synchronized read
     * pointer when their gray codes differ in the top two bits and
     * nowhere else. */
    always_comb pocket_fifo_full =
        wptr_gray == {~rptr_gray_w2[PW-1:PW-2], rptr_gray_w2[PW-3:0]};
    always_comb pocket_fifo_empty = rptr_gray == wptr_gray_r2;

    always_comb pocket_fifo_rdata = mem[rptr[DEPTH_LOG2-1:0]];

endmodule
