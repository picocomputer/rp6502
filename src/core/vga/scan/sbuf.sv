/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One sprite slot's line buffer, as two ping-pong banks. The banks are
 * separate arrays so each infers as a simple dual port: the engine drives
 * the write bank's write port, the erase side drives the scan bank's, and
 * the roles swap with wr_bank. One array with both writers would need a
 * true dual port.
 *
 * Each bank is two arrays, the even pixels and the odd, so an engine can
 * land two pixels a clock: the two are neighbours, so they are of
 * opposite parity and never meet at one array's write port.
 *
 * Bit 16 marks a pixel as written; the erase side clears it. sprite.sv
 * drives that side a pixel behind the one being displayed, so a bank is
 * already zero when it comes back to write duty and there is no clear
 * pass and no filled flag.
 */

module sbuf (
    input logic clk,

    input logic wr_bank,

    /* a_data[16:0] lands at a_addr under a_we[0] and a_data[33:17] at
     * a_addr + 1 under a_we[1]. */
    input logic [1:0] a_we,
    input logic [9:0] a_addr,
    input logic [33:0] a_data,

    input logic sc_we,
    input logic [9:0] sc_addr,

    input logic rd_en,
    input logic [9:0] rd_addr,
    input logic rd_bank,
    output logic [16:0] sbuf_pix
);

    (* ramstyle = "no_rw_check" *)
    logic [16:0] b0e[512];
    (* ramstyle = "no_rw_check" *)
    logic [16:0] b0o[512];
    (* ramstyle = "no_rw_check" *)
    logic [16:0] b1e[512];
    (* ramstyle = "no_rw_check" *)
    logic [16:0] b1o[512];

    /* Block RAM holds zeros once the FPGA is configured, so a bank reads
     * as transparent on the first frame; simulation has to agree. */
    initial
        for (int i = 0; i < 512; i++) begin
            b0e[i] = 17'd0;
            b0o[i] = 17'd0;
            b1e[i] = 17'd0;
            b1o[i] = 17'd0;
        end

    /* The engine's two pixels, sorted by parity. */
    logic ae_we, ao_we;
    logic [8:0] ae_addr, ao_addr;
    logic [16:0] ae_data, ao_data;
    always_comb begin
        if (a_addr[0]) begin
            ao_we = a_we[0];
            ao_addr = a_addr[9:1];
            ao_data = a_data[16:0];
            ae_we = a_we[1];
            ae_addr = a_addr[9:1] + 9'd1;
            ae_data = a_data[33:17];
        end else begin
            ae_we = a_we[0];
            ae_addr = a_addr[9:1];
            ae_data = a_data[16:0];
            ao_we = a_we[1];
            ao_addr = a_addr[9:1];
            ao_data = a_data[33:17];
        end
    end

    logic se_we, so_we;
    always_comb begin
        se_we = sc_we && !sc_addr[0];
        so_we = sc_we && sc_addr[0];
    end

    logic b0e_we, b0o_we, b1e_we, b1o_we;
    logic [8:0] b0e_addr, b0o_addr, b1e_addr, b1o_addr;
    logic [16:0] b0e_data, b0o_data, b1e_data, b1o_data;
    always_comb begin
        b0e_we = wr_bank ? se_we : ae_we;
        b0e_addr = wr_bank ? sc_addr[9:1] : ae_addr;
        b0e_data = wr_bank ? 17'd0 : ae_data;
        b0o_we = wr_bank ? so_we : ao_we;
        b0o_addr = wr_bank ? sc_addr[9:1] : ao_addr;
        b0o_data = wr_bank ? 17'd0 : ao_data;
        b1e_we = wr_bank ? ae_we : se_we;
        b1e_addr = wr_bank ? ae_addr : sc_addr[9:1];
        b1e_data = wr_bank ? ae_data : 17'd0;
        b1o_we = wr_bank ? ao_we : so_we;
        b1o_addr = wr_bank ? ao_addr : sc_addr[9:1];
        b1o_data = wr_bank ? ao_data : 17'd0;
    end

    always_ff @(posedge clk)
        if (b0e_we)
            b0e[b0e_addr] <= b0e_data;
    always_ff @(posedge clk)
        if (b0o_we)
            b0o[b0o_addr] <= b0o_data;
    always_ff @(posedge clk)
        if (b1e_we)
            b1e[b1e_addr] <= b1e_data;
    always_ff @(posedge clk)
        if (b1o_we)
            b1o[b1o_addr] <= b1o_data;

    /* Both banks are read every pixel and the bank and parity are
     * registered beside them, so the choice is a mux after the output
     * registers rather than logic in front of the memory. */
    logic [16:0] q0e, q0o, q1e, q1o;
    logic q_sel, q_odd;
    initial begin
        q0e = 17'd0;
        q0o = 17'd0;
        q1e = 17'd0;
        q1o = 17'd0;
        q_sel = 1'b0;
        q_odd = 1'b0;
    end
    always_ff @(posedge clk)
        if (rd_en) begin
            q0e <= b0e[rd_addr[9:1]];
            q0o <= b0o[rd_addr[9:1]];
            q1e <= b1e[rd_addr[9:1]];
            q1o <= b1o[rd_addr[9:1]];
            q_sel <= rd_bank;
            q_odd <= rd_addr[0];
        end
    always_comb sbuf_pix = q_sel ? (q_odd ? q1o : q1e)
                                 : (q_odd ? q0o : q0e);

endmodule
