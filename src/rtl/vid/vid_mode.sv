/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One plane's line buffer: ping-pong banks as two arrays, the engine
 * writing one while the beam reads and erases the other — the eraser
 * rides one pixel behind the read, so every bank returns to write duty
 * holding zeros, and a line nothing filled IS its zeros: black under
 * the base plane's rule, transparent under an overlay's. done arms the
 * bank flip and nothing else. Banks as two arrays because one array
 * with two writers needs a true dual port, and no conditional shape
 * survives extraction.
 */

module vid_mode (
    input logic clk,

    input logic [9:0] h,
    input logic px_last,
    input logic line_start,

    input logic px_we,
    input logic [9:0] px_addr,
    input logic [15:0] px_data,
    input logic done_i,

    output logic [15:0] vid_mode_pix
);

    (* ramstyle = "no_rw_check" *)
    logic [15:0] b0[1024];
    (* ramstyle = "no_rw_check" *)
    logic [15:0] b1[1024];

    /* The zeros are load-bearing from the first frame: hardware
     * configures block RAM to zero, and simulation must agree. */
    initial
        for (int i = 0; i < 1024; i++) begin
            b0[i] = 16'h0000;
            b1[i] = 16'h0000;
        end

    logic wr_bank;
    logic flip_next;

    logic b0_we, b1_we;
    logic [9:0] b0_addr, b1_addr;
    logic [15:0] b0_data, b1_data;
    logic sc_we;
    always_comb begin
        sc_we = !px_last;
        b0_we = wr_bank ? sc_we : px_we;
        b0_addr = wr_bank ? h - 10'd1 : px_addr;
        b0_data = wr_bank ? 16'h0000 : px_data;
        b1_we = wr_bank ? px_we : sc_we;
        b1_addr = wr_bank ? px_addr : h - 10'd1;
        b1_data = wr_bank ? px_data : 16'h0000;
    end

    always_ff @(posedge clk)
        if (b0_we)
            b0[b0_addr] <= b0_data;
    always_ff @(posedge clk)
        if (b1_we)
            b1[b1_addr] <= b1_data;

    /* The beam reads one ahead of itself, on each pixel's last tick, and
     * the bank flip lands on h==0's first tick — so only the pixel-0
     * read at the end of h==799 sees the fresh line under its write-side
     * label. Both banks read every pixel and the select is registered
     * beside them: a mux after the output registers costs fabric, not
     * the block-memory inference. */
    logic [9:0] rd_addr;
    logic rd_bank;
    always_comb begin
        rd_addr = h == 10'd799 ? 10'd0 : h + 10'd1;
        rd_bank = h == 10'd799 ? (flip_next ? wr_bank : !wr_bank)
                               : !wr_bank;
    end
    logic [15:0] q0, q1;
    logic q_sel;
    initial begin
        q0 = 16'h0000;
        q1 = 16'h0000;
        q_sel = 1'b0;
    end
    always_ff @(posedge clk)
        if (px_last) begin
            q0 <= b0[rd_addr];
            q1 <= b1[rd_addr];
            q_sel <= rd_bank;
        end
    always_comb vid_mode_pix = q_sel ? q1 : q0;

    initial begin
        wr_bank = 1'b0;
        flip_next = 1'b0;
    end
    /* The next line's pixel 0 is read during h==799, so the flip must
     * land before it or that pixel comes up stale. */
    always_ff @(posedge clk) begin
        if (line_start) begin
            if (flip_next)
                wr_bank <= !wr_bank;
            flip_next <= 1'b0;
        end else if (done_i)
            flip_next <= 1'b1;
    end

endmodule
