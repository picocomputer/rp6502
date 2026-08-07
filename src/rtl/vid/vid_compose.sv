/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The composite, scanvideo's exact rule: on the console canvas the
 * terminal is the whole picture; otherwise plane 0 is the base — shown
 * unconditionally when filled, black when not, alpha ignored the way
 * the base state machine ignores it — and fills and sprites stack
 * above, ascending, each landing where its alpha bit is set. Sprites
 * carry presence in bit 16 beside their own alpha in bit 5: the oracle
 * paints slot k into the highest filled plane at or below k, so a
 * present pixel with clear alpha still lands when that host is the
 * alpha-blind base — plane 0 filled with no filled plane between, and
 * for slot 0 always. A fill's filled flag guards its stale buffer;
 * sprite buffers need no flag because erased is absent. The letterbox
 * rows this used to blacken no longer exist: de never asserts outside
 * the canvas, so there is nothing here to paint over.
 */

module vid_compose (
    input logic clk,
    input logic de,
    input logic console,
    input logic [15:0] term_pix,
    input logic [15:0] p0_pix,
    input logic p0_filled,
    input logic [16:0] s0_pix,
    input logic [15:0] p1_pix,
    input logic p1_filled,
    input logic [16:0] s1_pix,
    input logic [15:0] p2_pix,
    input logic p2_filled,
    input logic [16:0] s2_pix,
    output logic [15:0] vid_compose_pix,
    output logic vid_compose_de
);

    logic [15:0] comp;
    always_comb begin
        if (console)
            comp = term_pix;
        else begin
            comp = p0_filled ? p0_pix : 16'h0000;
            if (s0_pix[16])
                comp = s0_pix[15:0];
            if (p1_filled && p1_pix[5])
                comp = p1_pix;
            if (s1_pix[16] && (s1_pix[5]
                               || (p0_filled && !p1_filled)))
                comp = s1_pix[15:0];
            if (p2_filled && p2_pix[5])
                comp = p2_pix;
            if (s2_pix[16] && (s2_pix[5]
                               || (p0_filled && !p1_filled && !p2_filled)))
                comp = s2_pix[15:0];
        end
    end

    initial begin
        vid_compose_pix = 16'h0000;
        vid_compose_de = 1'b0;
    end
    always_ff @(posedge clk) begin
        vid_compose_pix <= comp;
        vid_compose_de <= de;
    end

endmodule
