/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The fill engine's palette, shared by the three fill modes that can
 * hold the engine. One copy serves all three planes because sched runs
 * one plane at a time and a mode that reads the palette fetches every
 * entry it will index before it emits a pixel.
 *
 * Entries are split by the half of an XRAM word they arrive in, so that
 * the load writes a whole word, which holds two entries, in one clock.
 * The second copy answers mode 1's foreground and background in the
 * same cycle; modes 2 and 3 read port A alone. Both reads are
 * asynchronous because they have to answer where they are used.
 */

module palram
    import vid_palette_pkg::*;
(
    input logic clk,

    input logic ld,
    input logic [7:0] w,
    input logic half,
    input logic [31:0] a_rdata,

    /* The read answers with a finished color: the loaded entry, or the
     * built-in color the mode would otherwise have indexed itself.
     * VID_COLOR_256 is a 256-entry constant, so resolving it here builds
     * it twice, once a port, instead of once per reader. */
    input logic xram,
    input logic one_bpp,
    input logic [7:0] idx_a,
    input logic [7:0] idx_b,
    output logic [15:0] palram_qa,
    output logic [15:0] palram_qb
);

    /* Asynchronous reads rule out a block RAM, and left to itself
     * Quartus falls back to flip-flops rather than LUT memory: four
     * arrays of 128 sixteen-bit entries is 8192 flip-flops, and the
     * device stops fitting. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_a_lo[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_a_hi[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_b_lo[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_b_hi[128];

    /* Word w's halves are entries 2w and 2w + 1, or with a halfword-aligned
     * palette 2w - 1 and 2w, so its low half lands a line lower and the
     * reads flip halves. The halves either side of the palette land on
     * entries its depth never indexes, or on entry 255 before the load
     * reaches it; only the one past a 256-color palette would wrap onto
     * entry 0, and it is not written. */
    logic [6:0] wa_lo;
    always_comb wa_lo = half ? 7'(w - 8'd1) : w[6:0];

    /* No reset, because an array that takes one cannot be memory. */
    always_ff @(posedge clk) begin
        if (ld) begin
            pal_a_lo[wa_lo] <= a_rdata[15:0];
            pal_b_lo[wa_lo] <= a_rdata[15:0];
            if (!w[7]) begin
                pal_a_hi[w[6:0]] <= a_rdata[31:16];
                pal_b_hi[w[6:0]] <= a_rdata[31:16];
            end
        end
    end

    always_comb begin
        if (xram) begin
            palram_qa = idx_a[0] ^ half ? pal_a_hi[idx_a[7:1]]
                                        : pal_a_lo[idx_a[7:1]];
            palram_qb = idx_b[0] ^ half ? pal_b_hi[idx_b[7:1]]
                                        : pal_b_lo[idx_b[7:1]];
        end else if (one_bpp) begin
            palram_qa = VID_COLOR_2[idx_a[0]];
            palram_qb = VID_COLOR_2[idx_b[0]];
        end else begin
            palram_qa = VID_COLOR_256[idx_a];
            palram_qb = VID_COLOR_256[idx_b];
        end
    end

endmodule
