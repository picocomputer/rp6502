/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The sprite stage's palette cache. Mode 5 reads its palette from XRAM
 * live, per pixel — one arbitrated round trip per opaque pixel, which is
 * most of what a paletted sprite line costs. This turns those trips into
 * hits for the colors a row actually uses.
 *
 * Sixteen entries was the owner's spec, with one invariant: any sixteen
 * colors at any contiguous XRAM location must be conflict-free. Entries
 * are halfwords, words carry two, and a halfword-aligned palette spreads
 * sixteen colors across nine consecutive words — so the shape is eight
 * sets of two ways with one-word lines, and any run of sixteen or fewer
 * words maps at most two deep per set: a 4bpp sprite's whole palette
 * costs at most nine compulsory misses a row and can never thrash. A
 * miss is exactly today's fetch, so the pathological case is bounded by
 * what the stage already was — the cache only removes trips.
 *
 * The fill modes deliberately do NOT use this. A 256-color fill's
 * pathological line — indices cycling more colors than any small cache
 * holds — would miss per pixel and blow the deadline at any clock, and
 * the fill contract is deterministic completion. Fills keep their
 * snapshot store; the owner's requirement is what decided it.
 *
 * The read answers where it is used, like the store it replaces: tags and
 * data are flops, the hit and both colors are combinational, and the
 * builtin palettes resolve here so the consumer keeps getting a finished
 * color. The fill side is a master the plane's channel interposes ahead
 * of the mode engine's own fetches — the engine is stalled on the miss,
 * so the port is going spare.
 *
 * Coherence is by row, not by write — the owner's call: flush empties
 * the cache at each line's walk, so entries live for one row and a
 * palette write lands by the next. The C reads live per pixel, so a
 * write racing its own row's render is observed a row late here; that
 * write was always a race — the emulator renders a line at a moment of
 * its choosing, and this machine renders one line ahead of the beam.
 * Dropping the per-write snoop is what makes the cache this small.
 */

module vid_palcache
    import vid_palette_pkg::*;
(
    input logic clk,
    input logic rst_n,

    /* The consumer's question, and the finished answer. base is the
     * palette's XRAM byte address (bit 0 already validated clear); idx_b
     * rides beside idx_a for mode 1's foreground/background pair and
     * participates only when need_b. With xram clear the builtins answer
     * and there is nothing to miss. */
    input logic lookup,
    input logic xram,
    input logic one_bpp,
    input logic [15:0] base,
    input logic [7:0] idx_a,
    input logic [7:0] idx_b,
    input logic need_b,
    output logic [15:0] vid_palcache_qa,
    output logic [15:0] vid_palcache_qb,
    output logic vid_palcache_hit,

    /* The fill master. req holds until gnt; the word lands on a_rdata
     * the cycle rdy asserts, which is the interposer's grant-owner
     * bookkeeping, not a guess. */
    output logic vid_palcache_req,
    output logic [13:0] vid_palcache_addr,
    input logic fill_gnt,
    input logic fill_rdy,
    input logic [31:0] a_rdata,

    /* Row boundary: empty the cache, ending any fill mid-flight. */
    input logic flush
);

    /* Entry halfword address, then the word that holds it: the tag is
     * the word address above the set bits. */
    logic [15:0] ha_a, ha_b;
    always_comb ha_a = {1'b0, base[15:1]} + {8'd0, idx_a};
    always_comb ha_b = {1'b0, base[15:1]} + {8'd0, idx_b};
    logic [13:0] wa_a, wa_b;
    always_comb wa_a = ha_a[14:1];
    always_comb wa_b = ha_b[14:1];

    /* Eight sets, two ways, one word each. */
    logic [10:0] tag[8][2];
    logic valid[8][2];
    logic [31:0] line[8][2];
    logic lru[8]; /* which way to victimize next */

    logic [2:0] set_a, set_b;
    always_comb set_a = wa_a[2:0];
    always_comb set_b = wa_b[2:0];

    logic hit_a0, hit_a1, hit_b0, hit_b1, hit_a, hit_b;
    always_comb begin
        hit_a0 = valid[set_a][0] && tag[set_a][0] == wa_a[13:3];
        hit_a1 = valid[set_a][1] && tag[set_a][1] == wa_a[13:3];
        hit_b0 = valid[set_b][0] && tag[set_b][0] == wa_b[13:3];
        hit_b1 = valid[set_b][1] && tag[set_b][1] == wa_b[13:3];
        hit_a = hit_a0 || hit_a1;
        hit_b = hit_b0 || hit_b1;
    end

    logic [31:0] word_a, word_b;
    always_comb word_a = hit_a0 ? line[set_a][0] : line[set_a][1];
    always_comb word_b = hit_b0 ? line[set_b][0] : line[set_b][1];

    always_comb begin
        if (xram) begin
            vid_palcache_qa = ha_a[0] ? word_a[31:16] : word_a[15:0];
            vid_palcache_qb = ha_b[0] ? word_b[31:16] : word_b[15:0];
        end else if (one_bpp) begin
            vid_palcache_qa = VID_COLOR_2[idx_a[0]];
            vid_palcache_qb = VID_COLOR_2[idx_b[0]];
        end else begin
            vid_palcache_qa = VID_COLOR_256[idx_a];
            vid_palcache_qb = VID_COLOR_256[idx_b];
        end
    end
    always_comb vid_palcache_hit = !xram
        || (hit_a && (!need_b || hit_b));

    /* The fill: one word at a time, a's before b's. The request drops
     * the cycle the line lands, and the consumer's combinational hit
     * rises the same cycle the arrays update settle — one stalled pixel
     * per miss beyond the round trip itself. */
    logic filling;
    logic [13:0] fill_wa;
    always_comb begin
        vid_palcache_req = lookup && xram && !filling
            && (!hit_a || (need_b && !hit_b));
        vid_palcache_addr = !hit_a ? wa_a : wa_b;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int s = 0; s < 8; s++) begin
                valid[s][0] <= 1'b0;
                valid[s][1] <= 1'b0;
                lru[s] <= 1'b0;
            end
            filling <= 1'b0;
            fill_wa <= '0;
        end else begin
            if (!filling && fill_gnt) begin
                filling <= 1'b1;
                fill_wa <= vid_palcache_addr;
            end
            if (filling && fill_rdy) begin
                filling <= 1'b0;
                line[fill_wa[2:0]][lru[fill_wa[2:0]]] <= a_rdata;
                tag[fill_wa[2:0]][lru[fill_wa[2:0]]] <= fill_wa[13:3];
                valid[fill_wa[2:0]][lru[fill_wa[2:0]]] <= 1'b1;
                lru[fill_wa[2:0]] <= !lru[fill_wa[2:0]];
            end

            /* Touch on hit so the resident way survives. */
            if (lookup && xram && hit_a)
                lru[set_a] <= hit_a0;
            if (lookup && xram && need_b && hit_b)
                lru[set_b] <= hit_b0;

            /* The flush outranks a landing fill: a word granted before
             * the boundary must not resurrect after it. */
            if (flush) begin
                for (int s = 0; s < 8; s++) begin
                    valid[s][0] <= 1'b0;
                    valid[s][1] <= 1'b0;
                end
                filling <= 1'b0;
            end
        end
    end

    /* base[0] is validated clear before it arrives, and the halfword
     * sums cannot carry into bit 15 of a 64 KB space. */
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_palcache;
    always_comb unused_vid_palcache = ^{base[0], ha_a[15], ha_b[15]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
