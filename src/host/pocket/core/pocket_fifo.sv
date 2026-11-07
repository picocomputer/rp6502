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

    logic [PW-1:0] wr, wr_gray, rd, rd_gray;
    (* preserve *) logic [PW-1:0] rd_gray_w1, rd_gray_w2;
    (* preserve *) logic [PW-1:0] wr_gray_r1, wr_gray_r2;

    logic [PW-1:0] wr_n, rd_n;
    always_comb wr_n = wr + PW'(w_stb && !pocket_fifo_full);
    always_comb rd_n = rd + PW'(r_take && !pocket_fifo_empty);

    initial begin
        wr = '0;
        wr_gray = '0;
        rd_gray_w1 = '0;
        rd_gray_w2 = '0;
    end
    always_ff @(posedge wclk) begin
        if (w_stb && !pocket_fifo_full)
            mem[wr[DEPTH_LOG2-1:0]] <= w_data;
        wr <= wr_n;
        wr_gray <= wr_n ^ (wr_n >> 1);
        rd_gray_w1 <= rd_gray;
        rd_gray_w2 <= rd_gray_w1;
    end

    initial begin
        rd = '0;
        rd_gray = '0;
        wr_gray_r1 = '0;
        wr_gray_r2 = '0;
    end
    always_ff @(posedge rclk) begin
        rd <= rd_n;
        rd_gray <= rd_n ^ (rd_n >> 1);
        wr_gray_r1 <= wr_gray;
        wr_gray_r2 <= wr_gray_r1;
    end

    /* The write pointer is one lap ahead of the synchronized read
     * pointer when their gray codes differ in the top two bits and
     * nowhere else. */
    always_comb pocket_fifo_full =
        wr_gray == {~rd_gray_w2[PW-1:PW-2], rd_gray_w2[PW-3:0]};
    always_comb pocket_fifo_empty = rd_gray == wr_gray_r2;

    always_comb pocket_fifo_rdata = mem[rd[DEPTH_LOG2-1:0]];

endmodule
