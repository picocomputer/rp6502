/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One plane's palette, shared by the three renderers that can hold it.
 *
 * A plane runs exactly one of mode 1, 2 or 3 on any scanline, and they
 * can share because a mode that reads the palette always reloads it
 * first: the config decides pal_xram at the top of the line, and the
 * S_PAL state fetches every entry that mode will index before a pixel is
 * emitted. A mode that leaves pal_xram clear reads the built-in colors
 * and never looks here.
 *
 * The parity split gives the load one write per XRAM word, which carries
 * two entries. The second copy gives mode 1 a foreground and a
 * background entry in the same cycle; modes 2 and 3 read port A alone.
 * LUT RAM, because the read has to answer where it is used.
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
     * builtin the mode would otherwise have indexed itself. The builtin
     * is a 256-entry constant, and it was being synthesized once per
     * reader — four times a plane where twice will do. */
    input logic xram,
    input logic one_bpp,
    input logic [7:0] idx_a,
    input logic [7:0] idx_b,
    output logic [15:0] palram_qa,
    output logic [15:0] palram_qb
);

    /* Named, not inferred. Both reads are asynchronous, so a block is
     * out; but left to itself Quartus does not fall back to LUT memory,
     * it falls back to flip-flops — eight thousand of them an instance,
     * three instances, and the device stops fitting. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_a_even[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_a_odd[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_b_even[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_b_odd[128];

    /* A halfword-aligned palette puts entry 0 in the first word's high
     * half, so the ends of the run each write one side only. */
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

    /* Outside any reset, so it can be memory at all. */
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
