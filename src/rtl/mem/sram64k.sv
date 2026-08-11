/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module sram64k (
    input logic clk,

    input logic [15:0] a_addr,
    input logic [7:0] a_wdata,
    input logic a_we,
    output logic [7:0] a_rdata,

    input logic [15:0] b_addr,
    input logic [7:0] b_wdata,
    input logic b_we,
    output logic [7:0] b_rdata,

    input logic sst_own,
    input logic [15:0] sst_addr,
    input logic sst_we,
    input logic [7:0] sst_wdata
);

    (* ramstyle = "no_rw_check" *)
    logic [7:0] mem[65536] ;

    always_ff @(posedge clk) begin
        if (a_we && !sst_own)
            mem[a_addr] <= a_wdata;
        a_rdata <= mem[a_addr];
    end

    logic [15:0] b_a;
    logic b_w;
    logic [7:0] b_d;
    always_comb begin
        b_a = sst_own ? sst_addr : b_addr;
        b_w = sst_own ? sst_we : b_we;
        b_d = sst_own ? sst_wdata : b_wdata;
    end
    always_ff @(posedge clk) begin
        if (b_w)
            mem[b_a] <= b_d;
        b_rdata <= mem[b_a];
    end

endmodule
