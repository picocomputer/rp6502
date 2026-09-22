/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One plane's line buffer, as two ping-pong banks: the fill engine
 * writes one while the other is scanned out and erased a pixel behind
 * the one being displayed, so a bank holds zeros when it comes back to
 * write duty. done_i arms the bank flip and does nothing else. The banks are
 * separate arrays because one array with two writers would need a true
 * dual port.
 *
 * Each bank is two arrays, the even pixels and the odd, so the fill can
 * land two pixels a clock: the two are neighbours, so they are of
 * opposite parity and never meet at one array's write port.
 */

module linebuf (
    input logic clk,

    input logic [9:0] h,
    input logic px_last,
    input logic line_start,
    input logic flip_ok,
    input logic next_ok,

    /* px_data[15:0] lands at px_addr under px_we[0] and px_data[31:16]
     * at px_addr + 1 under px_we[1]. */
    input logic [1:0] px_we,
    input logic [9:0] px_addr,
    input logic [31:0] px_data,
    input logic done_i,

    output logic [15:0] linebuf_pix
);

    /* Block RAM holds zeros once the FPGA is configured, so an unfilled
     * line reads as zeros on the first frame; simulation has to agree. */
    (* ramstyle = "no_rw_check" *)
    logic [15:0] b0e[512];
    (* ramstyle = "no_rw_check" *)
    logic [15:0] b0o[512];
    (* ramstyle = "no_rw_check" *)
    logic [15:0] b1e[512];
    (* ramstyle = "no_rw_check" *)
    logic [15:0] b1o[512];
    initial
        for (int i = 0; i < 512; i++) begin
            b0e[i] = 16'h0000;
            b0o[i] = 16'h0000;
            b1e[i] = 16'h0000;
            b1o[i] = 16'h0000;
        end

    logic wr_bank;
    logic flip_next;

    /* The fill's two pixels, sorted by parity. */
    logic fe_we, fo_we;
    logic [8:0] fe_addr, fo_addr;
    logic [15:0] fe_data, fo_data;
    always_comb begin
        if (px_addr[0]) begin
            fo_we = px_we[0];
            fo_addr = px_addr[9:1];
            fo_data = px_data[15:0];
            fe_we = px_we[1];
            fe_addr = px_addr[9:1] + 9'd1;
            fe_data = px_data[31:16];
        end else begin
            fe_we = px_we[0];
            fe_addr = px_addr[9:1];
            fe_data = px_data[15:0];
            fo_we = px_we[1];
            fo_addr = px_addr[9:1];
            fo_data = px_data[31:16];
        end
    end

    /* The erase, a pixel behind the beam. */
    logic [9:0] sc_px;
    logic se_we, so_we;
    always_comb begin
        sc_px = h - 10'd1;
        se_we = !px_last && !sc_px[0];
        so_we = !px_last && sc_px[0];
    end

    logic b0e_we, b0o_we, b1e_we, b1o_we;
    logic [8:0] b0e_addr, b0o_addr, b1e_addr, b1o_addr;
    logic [15:0] b0e_data, b0o_data, b1e_data, b1o_data;
    always_comb begin
        b0e_we = wr_bank ? se_we : fe_we;
        b0e_addr = wr_bank ? sc_px[9:1] : fe_addr;
        b0e_data = wr_bank ? 16'h0000 : fe_data;
        b0o_we = wr_bank ? so_we : fo_we;
        b0o_addr = wr_bank ? sc_px[9:1] : fo_addr;
        b0o_data = wr_bank ? 16'h0000 : fo_data;
        b1e_we = wr_bank ? fe_we : se_we;
        b1e_addr = wr_bank ? fe_addr : sc_px[9:1];
        b1e_data = wr_bank ? fe_data : 16'h0000;
        b1o_we = wr_bank ? fo_we : so_we;
        b1o_addr = wr_bank ? fo_addr : sc_px[9:1];
        b1o_data = wr_bank ? fo_data : 16'h0000;
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

    /* The read for the next pixel lands on the last clock of this one,
     * from the bank that will be scanned then: the bank flip is on
     * h==0's first clock, so only the pixel-0 read at the end of h==799
     * has to take the bank the flip is about to make the scan bank. */
    logic [9:0] rd_addr;
    logic rd_bank;
    always_comb begin
        rd_addr = h == 10'd799 ? 10'd0 : h + 10'd1;
        rd_bank = h == 10'd799 && next_ok ? (flip_next ? wr_bank : !wr_bank)
                                         : !wr_bank;
    end
    logic [15:0] q0e, q0o, q1e, q1o;
    logic q_sel, q_odd;
    initial begin
        q0e = 16'h0000;
        q0o = 16'h0000;
        q1e = 16'h0000;
        q1o = 16'h0000;
        q_sel = 1'b0;
        q_odd = 1'b0;
    end
    always_ff @(posedge clk)
        if (px_last) begin
            q0e <= b0e[rd_addr[9:1]];
            q0o <= b0o[rd_addr[9:1]];
            q1e <= b1e[rd_addr[9:1]];
            q1o <= b1o[rd_addr[9:1]];
            q_sel <= rd_bank;
            q_odd <= rd_addr[0];
        end
    always_comb linebuf_pix = q_sel ? (q_odd ? q1o : q1e)
                                    : (q_odd ? q0o : q0e);

    initial begin
        wr_bank = 1'b0;
        flip_next = 1'b0;
    end
    always_ff @(posedge clk) begin
        if (line_start && flip_ok) begin
            if (flip_next)
                wr_bank <= !wr_bank;
            flip_next <= 1'b0;
        end else if (done_i)
            flip_next <= 1'b1;
    end

endmodule
