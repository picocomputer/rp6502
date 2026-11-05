/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One of pcm's four FIFO banks: 1024 bytes in M10K. Port A on clk_a
 * writes and reads back at waddr, port B on clk is the player's
 * registered read with a read enable.
 */

module pcm_bank (
    input logic clk_a,
    input logic clk,

    input logic we,
    input logic [9:0] waddr,
    input logic [7:0] wdata,
    output logic [7:0] pcm_bank_a_q,

    input logic re,
    input logic [9:0] raddr,
    output logic [7:0] pcm_bank_q
);

    (* ramstyle = "M10K, no_rw_check" *)
    logic [7:0] mem[1024];

    always_ff @(posedge clk_a) begin
        if (we)
            mem[waddr] <= wdata;
        pcm_bank_a_q <= mem[waddr];
    end
    always_ff @(posedge clk)
        if (re)
            pcm_bank_q <= mem[raddr];

endmodule
