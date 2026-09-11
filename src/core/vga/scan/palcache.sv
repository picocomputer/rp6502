/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The sprite stage's palette cache. Mode 5 reads its palette from XRAM
 * for every pixel, transparent ones included, because the alpha bit is
 * in the color the lookup returns.
 *
 * A 4bpp palette anywhere in XRAM has to be conflict-free. A
 * halfword-aligned one spreads sixteen colors across nine consecutive
 * words, and eight sets of two ways with one-word lines hold any run of
 * nine words at most two deep in a set, so a sixteen-color sprite takes
 * nine compulsory misses a row and cannot thrash. An 8bpp sprite ranging
 * over more than sixteen words can. A miss costs the fetch it replaces,
 * so the cache only ever removes trips.
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

    /* With xram clear the built-in colors answer and nothing can miss. */
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
            palcache_qa = ha_a[0] ? word_a[31:16] : word_a[15:0];
            palcache_qb = ha_b[0] ? word_b[31:16] : word_b[15:0];
        end else if (one_bpp) begin
            palcache_qa = VID_COLOR_2[idx_a[0]];
            palcache_qb = VID_COLOR_2[idx_b[0]];
        end else begin
            palcache_qa = VID_COLOR_256[idx_a];
            palcache_qb = VID_COLOR_256[idx_b];
        end
    end
    always_comb palcache_hit = !xram
        || (hit_a && (!need_b || hit_b));

    /* The request is registered rather than driven straight from the tag
     * compare, because the lookup's address arithmetic already fills a
     * clock and reaching the XRAM's address port in the same one would
     * put the two in series. The miss pays a clock it was going to spend
     * stalled anyway. */
    logic pending;
    logic [13:0] pend_wa;
    logic filling;
    logic [13:0] fill_wa;
    logic miss_now;
    always_comb miss_now = lookup && xram && !filling && !pending
        && (!hit_a || (need_b && !hit_b));
    always_comb begin
        palcache_req = pending;
        palcache_addr = pend_wa;
    end

    initial begin
        for (int s = 0; s < 8; s++) begin
            valid[s][0] = 1'b0;
            valid[s][1] = 1'b0;
            lru[s] = 1'b0;
        end
        pending = 1'b0;
        pend_wa = '0;
        filling = 1'b0;
        fill_wa = '0;
    end
    always_ff @(posedge clk) begin
        if (miss_now) begin
            pending <= 1'b1;
            pend_wa <= !hit_a ? wa_a : wa_b;
        end
        if (pending && fill_gnt) begin
            pending <= 1'b0;
            filling <= 1'b1;
            fill_wa <= pend_wa;
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

        /* The flush is written last, so it outranks a fill landing on
         * the same clock: a word granted before the row boundary must
         * not validate a way after it. */
        if (flush) begin
            for (int s = 0; s < 8; s++) begin
                valid[s][0] <= 1'b0;
                valid[s][1] <= 1'b0;
            end
            pending <= 1'b0;
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
