/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The fixed rule that stacks the three planes. The alpha bit is bit 5 of
 * a pixel, the one the RGB555 color leaves spare
 * (core/vga/pixel_format.h).
 *
 * Within a plane, a sprite pixel replaces the fill's wherever the sprite
 * buffer marks one written (bit 16), whether or not that pixel's alpha
 * bit is set, so a sprite pixel with alpha clear punches a hole through
 * an overlay plane.
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
