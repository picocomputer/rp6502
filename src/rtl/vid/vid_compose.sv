/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The plane composite, emu/sys/vga.c's exact rule: on the console canvas
 * the terminal is the whole picture; otherwise the lowest filled plane is
 * the base, composed unconditionally, and the planes above it overwrite
 * only where their alpha bit is set, ascending. Nothing filled is opaque
 * black, the same black the oracle paints where nothing covers. The
 * letterbox rows this used to blacken no longer exist: de never asserts
 * outside the canvas, so there is nothing here to paint over.
 */

module vid_compose (
    input logic clk,
    input logic de,
    input logic console,
    input logic [15:0] term_pix,
    input logic [15:0] p0_pix,
    input logic p0_filled,
    input logic [15:0] p1_pix,
    input logic p1_filled,
    input logic [15:0] p2_pix,
    input logic p2_filled,
    output logic [15:0] vid_compose_pix,
    output logic vid_compose_de
);

    logic [15:0] comp;
    always_comb begin
        if (console)
            comp = term_pix;
        else begin
            comp = 16'h0000;
            if (p0_filled)
                comp = p0_pix;
            else if (p1_filled)
                comp = p1_pix;
            else if (p2_filled)
                comp = p2_pix;
            if (p1_filled && p0_filled && p1_pix[5])
                comp = p1_pix;
            if (p2_filled && (p0_filled || p1_filled) && p2_pix[5])
                comp = p2_pix;
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
