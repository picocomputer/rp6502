/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One sprite slot's line buffer: ping-pong banks as two arrays, so each
 * is a simple dual port the tool infers — the engine owns the write
 * bank's write port, the eraser owns the scan bank's, and the roles
 * swap with wr_bank. One array with both writers needs a true dual
 * port, and no conditional shape survived extraction. The eraser rides
 * the beam one pixel behind the scanout read, so every bank returns to
 * write duty already erased — no filled flag and no clear pass. Bit 16
 * is presence — written pixels carry it, the eraser clears it — and it
 * rides free: seventeen wide is the same block count as sixteen.
 */

module sbuf (
    input logic clk,

    input logic wr_bank,

    input logic a_we,
    input logic [9:0] a_addr,
    input logic [16:0] a_data,

    input logic sc_we,
    input logic [9:0] sc_addr,

    input logic rd_en,
    input logic [9:0] rd_addr,
    input logic rd_bank,
    output logic [16:0] sbuf_pix
);

    (* ramstyle = "no_rw_check" *)
    logic [16:0] b0[1024];
    (* ramstyle = "no_rw_check" *)
    logic [16:0] b1[1024];

    /* The zeros are load-bearing from the first frame: hardware
     * configures block RAM to zero, and simulation must agree. */
    initial
        for (int i = 0; i < 1024; i++) begin
            b0[i] = 17'd0;
            b1[i] = 17'd0;
        end

    logic b0_we, b1_we;
    logic [9:0] b0_addr, b1_addr;
    logic [16:0] b0_data, b1_data;
    always_comb begin
        b0_we = wr_bank ? sc_we : a_we;
        b0_addr = wr_bank ? sc_addr : a_addr;
        b0_data = wr_bank ? 17'd0 : a_data;
        b1_we = wr_bank ? a_we : sc_we;
        b1_addr = wr_bank ? a_addr : sc_addr;
        b1_data = wr_bank ? a_data : 17'd0;
    end

    always_ff @(posedge clk)
        if (b0_we)
            b0[b0_addr] <= b0_data;
    always_ff @(posedge clk)
        if (b1_we)
            b1[b1_addr] <= b1_data;

    /* Both banks read every pixel and the select is registered beside
     * them: a mux after the output registers costs fabric, not the
     * block-memory inference. */
    logic [16:0] q0, q1;
    logic q_sel;
    initial begin
        q0 = 17'd0;
        q1 = 17'd0;
        q_sel = 1'b0;
    end
    always_ff @(posedge clk)
        if (rd_en) begin
            q0 <= b0[rd_addr];
            q1 <= b1[rd_addr];
            q_sel <= rd_bank;
        end
    always_comb sbuf_pix = q_sel ? q1 : q0;

endmodule
