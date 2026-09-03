/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The composite, the three state machines' fixed rule: plane 0 always
 * drives, alpha-blind; planes 1 and 2 land where their own alpha bit is
 * set. Within a plane, a sprite's written pixel replaces the fill's —
 * the in-buffer overwrite — so an alpha-clear sprite texel replaces the
 * fill pixel and then fails the gate, the hardware's hole punch. There
 * is no filled: an unfilled line is its zeros, black under plane 0's
 * rule and transparent under the overlays'.
 */

module compose (
    input logic clk,
    input logic de,
    input logic [15:0] p0_pix,
    input logic [16:0] s0_pix,
    input logic [15:0] p1_pix,
    input logic [16:0] s1_pix,
    input logic [15:0] p2_pix,
    input logic [16:0] s2_pix,
    output logic [15:0] compose_pix,
    output logic compose_de
);

    logic [15:0] l0, l1, l2;
    logic [15:0] comp;
    always_comb begin
        l0 = s0_pix[16] ? s0_pix[15:0] : p0_pix;
        l1 = s1_pix[16] ? s1_pix[15:0] : p1_pix;
        l2 = s2_pix[16] ? s2_pix[15:0] : p2_pix;
        comp = l0;
        if (l1[5])
            comp = l1;
        if (l2[5])
            comp = l2;
    end

    initial begin
        compose_pix = 16'h0000;
        compose_de = 1'b0;
    end
    always_ff @(posedge clk) begin
        compose_pix <= comp;
        compose_de <= de;
    end

endmodule
