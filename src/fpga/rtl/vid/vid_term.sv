/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The terminal's cell memory and scanout registers. The cells are the four
 * screen buffers of vga/term/term.c — 61,440 bytes the linker places behind
 * the soft CPU's 0x5 window, so the shared ANSI engine's ordinary stores
 * land here unchanged. The register bank above them carries what the
 * renderer needs each frame: the visible rows' base offsets (term.c owns
 * the O(1) scroll remap and the firmware publishes the resolved bases once
 * per frame), the cursor, the blink phase, and the prog window.
 *
 * Registers written by the firmware are shadows; the scanout latches them
 * at frame start, one frame of latency and never a tear. The scanout side
 * arrives with vid_compose; until then the shadows and the frame counter
 * are the whole story.
 *
 * The cells power up zero — BRAM contents ship in the bitstream, so there
 * is no boot-time clear; term.c's own lazy row clears do the rest.
 */

module vid_term (
    input logic clk,
    input logic rst_n,
    input logic frame_start,

    /* The soft CPU's window: bit 16 low is cell memory, word-wide with
     * byte lanes; bit 16 high is the register bank. */
    input logic b_stb,
    input logic b_we,
    input logic [16:0] b_addr,
    input logic [3:0] b_wstrb,
    input logic [31:0] b_wdata,
    output logic [31:0] vid_term_b_rdata
);

    logic [31:0] cells[15360] /*verilator public_flat_rw*/;

    logic [13:0] cell_idx;
    always_comb cell_idx = b_addr[15:2];

    /* Row bases as byte offsets into the cell window, cursor packed
     * {enabled[25], lit[24], style[23:16], y[15:8], x[7:0]}, the cursor
     * color, the blink phase, and prog {enable[31], end[25:16], begin[9:0]}. */
    logic [15:0] row_shadow[32] /*verilator public_flat_rd*/;
    logic [31:0] cursor_shadow /*verilator public_flat_rd*/;
    logic [15:0] cursor_color_shadow /*verilator public_flat_rd*/;
    logic [1:0] blink_shadow /*verilator public_flat_rd*/;
    logic [31:0] prog_shadow /*verilator public_flat_rd*/;
    logic [31:0] frame_count /*verilator public_flat_rd*/;

    always_ff @(posedge clk) begin
        if (b_stb) begin
            if (!b_addr[16]) begin
                vid_term_b_rdata <= cells[cell_idx];
                if (b_we) begin
                    if (b_wstrb[0])
                        cells[cell_idx][7:0] <= b_wdata[7:0];
                    if (b_wstrb[1])
                        cells[cell_idx][15:8] <= b_wdata[15:8];
                    if (b_wstrb[2])
                        cells[cell_idx][23:16] <= b_wdata[23:16];
                    if (b_wstrb[3])
                        cells[cell_idx][31:24] <= b_wdata[31:24];
                end
            end else begin
                case (b_addr[7:2])
                    6'd32: vid_term_b_rdata <= cursor_shadow;
                    6'd33: vid_term_b_rdata <= {16'd0, cursor_color_shadow};
                    6'd34: vid_term_b_rdata <= {30'd0, blink_shadow};
                    6'd35: vid_term_b_rdata <= prog_shadow;
                    6'd40: vid_term_b_rdata <= frame_count;
                    default: vid_term_b_rdata <=
                        {16'd0, row_shadow[b_addr[6:2]]};
                endcase
            end
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int i = 0; i < 32; i++)
                row_shadow[i] <= 16'h0000;
            cursor_shadow <= 32'h0;
            cursor_color_shadow <= 16'h0;
            blink_shadow <= 2'h0;
            prog_shadow <= 32'h0;
            frame_count <= 32'h0;
        end else begin
            if (frame_start)
                frame_count <= frame_count + 32'd1;
            if (b_stb && b_we && b_addr[16]) begin
                case (b_addr[7:2])
                    6'd32: cursor_shadow <= b_wdata;
                    6'd33: cursor_color_shadow <= b_wdata[15:0];
                    6'd34: blink_shadow <= b_wdata[1:0];
                    6'd35: prog_shadow <= b_wdata;
                    6'd40: ;  /* the frame counter is the raster's */
                    default: begin
                        if (!b_addr[7])
                            row_shadow[b_addr[6:2]] <= b_wdata[15:0];
                    end
                endcase
            end
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_term;
    always_comb unused_vid_term = ^{b_addr[1:0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
