/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The sprite stage's palette cache. Mode 5 reads its palette from XRAM
 * for every pixel, transparent ones included, because the alpha bit is
 * in the color the lookup returns.
 *
 * A kilobyte of palette, 256 one-word lines picked by the word's address,
 * so any 256 consecutive words of XRAM map onto the lines one to one:
 * palettes kept inside one contiguous kilobyte never evict each other,
 * and a row fetches each word it uses once. Palettes a kilobyte apart
 * share lines and reload each other. A miss costs the fetch it replaces,
 * so the cache only ever removes trips.
 *
 * The colors live in two M10Ks, one holding each line's low color and
 * the other its high, each with a port for each of the pair's lookups,
 * and a color comes out the clock after its lookup. The tags are in
 * MLAB, read in the lookup's own clock, so hit or miss is known at once
 * and a miss asks that clock. The word landing answers the lookup for it
 * as it lands, and is written in through the ports that lookup would
 * have read by.
 *
 * The fill modes deliberately do not use this. A 256-color fill cycling
 * through more colors than the cache holds would miss on every pixel and
 * blow the deadline the fill modes are contracted to meet.
 *
 * The built-in palettes resolve here, so the consumer always gets a
 * finished color.
 *
 * Coherence is by row rather than by write: flush empties the cache each
 * line, so a palette write lands by the next row and there is no need to
 * snoop writes.
 */

module palcache
    import vid_palette_pkg::*;
(
    input logic clk,

    /* With xram clear the built-in colors answer and nothing can miss.
     * palcache_hit answers in the lookup's own clock; the colors come
     * the clock after. */
    input logic lookup,
    input logic xram,
    input logic one_bpp,
    input logic [15:0] base,
    input logic [7:0] idx_a,
    input logic [7:0] idx_b,
    input logic need_b,
    output logic [15:0] palcache_qa,
    output logic [15:0] palcache_qb,
    output logic palcache_hit,

    output logic palcache_req,
    output logic [13:0] palcache_addr,
    input logic fill_gnt,
    input logic fill_rdy,
    input logic [31:0] a_rdata,

    /* The row boundary. It empties the cache and ends any fill in
     * flight. */
    input logic flush
);

    logic [15:0] ha_a, ha_b;
    always_comb ha_a = {1'b0, base[15:1]} + {8'd0, idx_a};
    always_comb ha_b = {1'b0, base[15:1]} + {8'd0, idx_b};
    logic [13:0] wa_a, wa_b;
    always_comb wa_a = ha_a[14:1];
    always_comb wa_b = ha_b[14:1];

    /* A copy of the tags for each lookup, because an MLAB reads through
     * one port. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [5:0] tag_a[256];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [5:0] tag_b[256];
    logic [255:0] valid;

    logic filling;
    logic [13:0] fill_wa;
    logic land, land_a, land_b, hit_a, hit_b;
    always_comb begin
        land = filling && fill_rdy;
        land_a = land && fill_wa == wa_a;
        land_b = land && fill_wa == wa_b;
        hit_a = (valid[wa_a[7:0]] && tag_a[wa_a[7:0]] == wa_a[13:8])
            || land_a;
        hit_b = (valid[wa_b[7:0]] && tag_b[wa_b[7:0]] == wa_b[13:8])
            || land_b;
    end
    always_comb palcache_hit = !xram
        || (hit_a && (!need_b || hit_b));

    /* The request comes straight off the compare, one word in flight,
     * and the clock a word lands the other index may ask for its own. */
    logic miss_now;
    always_comb miss_now = lookup && xram && (!filling || land)
        && (!hit_a || (need_b && !hit_b));
    always_comb begin
        palcache_req = miss_now;
        palcache_addr = !hit_a ? wa_a : wa_b;
    end

    /* The colors, each line's low one in lo and its high in hi. Each
     * block's ports read for the two lookups, and the landing word is
     * written through the ports of the lookup it answers, which reads
     * nothing that clock. */
    (* ramstyle = "M10K, no_rw_check" *)
    logic [15:0] lo[256];
    (* ramstyle = "M10K, no_rw_check" *)
    logic [15:0] hi[256];
    logic [15:0] lo_qa, hi_qa, lo_qb, hi_qb;
    logic wr_a, wr_b;
    always_comb begin
        wr_a = land && land_a;
        wr_b = land && !land_a;
    end
    initial
        for (int i = 0; i < 256; i++) begin
            lo[i] = 16'd0;
            hi[i] = 16'd0;
            tag_a[i] = 6'd0;
            tag_b[i] = 6'd0;
        end
    initial valid = '0;
    always_ff @(posedge clk) begin
        if (wr_a) begin
            lo[fill_wa[7:0]] <= a_rdata[15:0];
            hi[fill_wa[7:0]] <= a_rdata[31:16];
        end else begin
            lo_qa <= lo[wa_a[7:0]];
            hi_qa <= hi[wa_a[7:0]];
        end
        if (wr_b) begin
            lo[fill_wa[7:0]] <= a_rdata[15:0];
            hi[fill_wa[7:0]] <= a_rdata[31:16];
        end else begin
            lo_qb <= lo[wa_b[7:0]];
            hi_qb <= hi[wa_b[7:0]];
        end
    end

    /* What answers the clock after: the block's color, the landing
     * word's, or a built-in palette's. */
    logic half_a, half_b, byp_a, byp_b, xram_q;
    logic [15:0] byp_qa, byp_qb, bi_qa, bi_qb;
    initial begin
        lo_qa = '0;
        hi_qa = '0;
        lo_qb = '0;
        hi_qb = '0;
        half_a = 1'b0;
        half_b = 1'b0;
        byp_a = 1'b0;
        byp_b = 1'b0;
        xram_q = 1'b0;
        byp_qa = '0;
        byp_qb = '0;
        bi_qa = '0;
        bi_qb = '0;
        filling = 1'b0;
        fill_wa = '0;
    end
    always_ff @(posedge clk) begin
        half_a <= ha_a[0];
        half_b <= ha_b[0];
        byp_a <= land_a;
        byp_b <= land_b;
        byp_qa <= ha_a[0] ? a_rdata[31:16] : a_rdata[15:0];
        byp_qb <= ha_b[0] ? a_rdata[31:16] : a_rdata[15:0];
        xram_q <= xram;
        bi_qa <= one_bpp ? VID_COLOR_2[idx_a[0]] : VID_COLOR_256[idx_a];
        bi_qb <= one_bpp ? VID_COLOR_2[idx_b[0]] : VID_COLOR_256[idx_b];
    end
    always_comb begin
        palcache_qa = !xram_q ? bi_qa
            : byp_a ? byp_qa : half_a ? hi_qa : lo_qa;
        palcache_qb = !xram_q ? bi_qb
            : byp_b ? byp_qb : half_b ? hi_qb : lo_qb;
    end

    always_ff @(posedge clk) begin
        if (land) begin
            filling <= 1'b0;
            tag_a[fill_wa[7:0]] <= fill_wa[13:8];
            tag_b[fill_wa[7:0]] <= fill_wa[13:8];
            valid[fill_wa[7:0]] <= 1'b1;
        end
        if (miss_now && fill_gnt) begin
            filling <= 1'b1;
            fill_wa <= palcache_addr;
        end

        /* The flush is written last, so it outranks a fill landing on
         * the same clock: a word granted before the row boundary must
         * not validate a line after it. */
        if (flush) begin
            valid <= '0;
            filling <= 1'b0;
        end
    end

    /* mode5 only sets xram for a halfword-aligned palette that fits
     * inside the 64 KB of XRAM, so base[0] is clear and base/2 plus an
     * index cannot reach bit 15. */
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_palcache;
    always_comb unused_palcache = ^{base[0], ha_a[15], ha_b[15]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
