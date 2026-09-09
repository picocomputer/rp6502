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
 * Entries are split by parity so that the load writes one XRAM word,
 * which holds two entries, in one clock. The second copy answers mode
 * 1's foreground and background in the same cycle; modes 2 and 3 read
 * port A alone. Both reads are asynchronous because they have to answer
 * where they are used.
 */

module palram
    import vid_palette_pkg::*;
(
    input logic clk,

    input logic ld,
    input logic [7:0] w,
    input logic [8:0] words,
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
    logic [15:0] pal_a_even[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_a_odd[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_b_even[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_b_odd[128];

    /* A halfword-aligned palette puts entry 0 in the first word's high
     * half, so each end of the run writes one parity only: the first
     * word's low half is the halfword before the palette, and the last
     * word's high half is the halfword after it. */
    logic we_e, we_o;
    logic [6:0] wa_o;
    logic [15:0] wd_e, wd_o;
    always_comb begin
        we_e = ld && (!half || {1'b0, w} != words);
        we_o = ld && (!half || w != 8'd0);
        wa_o = half ? 7'(w - 8'd1) : w[6:0];
        wd_e = half ? a_rdata[31:16] : a_rdata[15:0];
        wd_o = half ? a_rdata[15:0] : a_rdata[31:16];
    end

    /* No reset, because an array that takes one cannot be memory. */
    always_ff @(posedge clk) begin
        if (we_e) begin
            pal_a_even[w[6:0]] <= wd_e;
            pal_b_even[w[6:0]] <= wd_e;
        end
        if (we_o) begin
            pal_a_odd[wa_o] <= wd_o;
            pal_b_odd[wa_o] <= wd_o;
        end
    end

    always_comb begin
        if (xram) begin
            palram_qa = idx_a[0] ? pal_a_odd[idx_a[7:1]]
                                     : pal_a_even[idx_a[7:1]];
            palram_qb = idx_b[0] ? pal_b_odd[idx_b[7:1]]
                                     : pal_b_even[idx_b[7:1]];
        end else if (one_bpp) begin
            palram_qa = VID_COLOR_2[idx_a[0]];
            palram_qb = VID_COLOR_2[idx_b[0]];
        end else begin
            palram_qa = VID_COLOR_256[idx_a];
            palram_qb = VID_COLOR_256[idx_b];
        end
    end

endmodule
