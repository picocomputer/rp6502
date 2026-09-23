/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One plane's line buffer, as two ping-pong banks: the fill engine
 * writes one while the other is scanned out. done_i arms the bank flip,
 * and a bank shows only for a line it was flipped in for, because a fill
 * that finishes has written every pixel of the canvas; any other line
 * scans out zeros. The banks are separate arrays because one array with
 * two writers would need a true dual port.
 *
 * Each bank is two arrays, the even pixels and the odd, so the fill can
 * land two pixels a clock: the two are neighbours, so they are of
 * opposite parity and never meet at one array's write port.
 */

module linebuf (
    input logic clk,

    /* The beam's derived columns, computed once at the top level because
     * all three planes share them: the read for the next pixel wraps at
     * the line's end. */
    input logic [9:0] rd_addr,
    input logic h_last,

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

    (* ramstyle = "no_rw_check" *)
    logic [15:0] b0e[512];
    (* ramstyle = "no_rw_check" *)
    logic [15:0] b0o[512];
    (* ramstyle = "no_rw_check" *)
    logic [15:0] b1e[512];
    (* ramstyle = "no_rw_check" *)
    logic [15:0] b1o[512];

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

    always_ff @(posedge clk)
        if (fe_we && !wr_bank)
            b0e[fe_addr] <= fe_data;
    always_ff @(posedge clk)
        if (fo_we && !wr_bank)
            b0o[fo_addr] <= fo_data;
    always_ff @(posedge clk)
        if (fe_we && wr_bank)
            b1e[fe_addr] <= fe_data;
    always_ff @(posedge clk)
        if (fo_we && wr_bank)
            b1o[fo_addr] <= fo_data;

    /* The read for the next pixel lands on the last clock of this one,
     * from the bank that will be scanned then: the bank flip is on
     * h==0's first clock, so only the pixel-0 read at the end of h==799
     * has to take the bank the flip is about to make the scan bank. */
    logic rd_bank;
    always_comb rd_bank = h_last && next_ok ? (flip_next ? wr_bank : !wr_bank)
                                            : !wr_bank;
    /* shown says the scan bank was flipped in for this line. A 320 wide
     * fill can be flipped in on the first line of a 640 wide canvas, the
     * canvas having changed while it ran, and narrow keeps the right half
     * of that line at zeros. The fill's address on its done clock is the
     * width it ran to. */
    logic shown, narrow, narrow_next;
    logic [15:0] q0e, q0o, q1e, q1o;
    logic q_sel, q_odd, q_v;
    initial begin
        q0e = 16'h0000;
        q0o = 16'h0000;
        q1e = 16'h0000;
        q1o = 16'h0000;
        q_sel = 1'b0;
        q_odd = 1'b0;
        q_v = 1'b0;
    end
    always_ff @(posedge clk)
        if (px_last) begin
            q0e <= b0e[rd_addr[9:1]];
            q0o <= b0o[rd_addr[9:1]];
            q1e <= b1e[rd_addr[9:1]];
            q1o <= b1o[rd_addr[9:1]];
            q_sel <= rd_bank;
            q_odd <= rd_addr[0];
            q_v <= (h_last && next_ok ? flip_next : shown)
                && !(narrow && rd_addr >= 10'd320);
        end
    always_comb linebuf_pix = !q_v ? 16'h0000
        : q_sel ? (q_odd ? q1o : q1e)
                : (q_odd ? q0o : q0e);

    initial begin
        wr_bank = 1'b0;
        flip_next = 1'b0;
        shown = 1'b0;
        narrow = 1'b0;
        narrow_next = 1'b0;
    end
    always_ff @(posedge clk) begin
        if (line_start && flip_ok) begin
            if (flip_next)
                wr_bank <= !wr_bank;
            flip_next <= 1'b0;
            shown <= flip_next;
            narrow <= narrow_next;
        end else if (done_i) begin
            flip_next <= 1'b1;
            narrow_next <= !px_addr[9];
        end
    end

endmodule
