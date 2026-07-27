/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The plane composite, one plane deep so far. The scanvideo pixel word is
 * RGB555 with alpha at bit 5, but the base plane composes unconditionally
 * — emu/sys/vga.c gates alpha only on the planes above it — and an
 * unfilled line is zeros, which is the same opaque black the emulator
 * paints where nothing covers.
 */

module vid_compose (
    input logic clk,
    input logic rst_n,
    input logic de,
    input logic [15:0] plane0,
    output logic [15:0] vid_compose_pix,
    output logic vid_compose_de
);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            vid_compose_pix <= 16'h0000;
            vid_compose_de <= 1'b0;
        end else begin
            vid_compose_pix <= plane0;
            vid_compose_de <= de;
        end
    end

endmodule
