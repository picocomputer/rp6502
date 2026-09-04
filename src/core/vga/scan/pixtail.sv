/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pixel tail every fill mode shares, so pixel-exact detail exists
 * once. A line is a sequence of segments: an xram segment is a bit
 * origin and a pixel count, an immediate segment is eight bits and two
 * finished colours.
 *
 * That split is what makes the modes ordinary. A font row IS a 1bpp
 * bitmap and a cell's fg/bg IS a two-entry palette, so mode 1 is eighty
 * immediate segments; a mode 3 wraparound just ends one segment and
 * starts the next; transparent padding is an immediate segment of zeros,
 * so out-of-window and blank lines are not special cases.
 *
 * The front owns geometry, the tail owns the pixel. Segments hand over
 * without a bubble while the front stays a segment ahead.
 */

module pixtail
    import vid_palette_pkg::*;
(
    input logic clk,

    input logic start,
    input logic abort_i,
    input logic [9:0] cw,

    /* The palette plan, valid at start. bpp_log 0-3 index the palette;
     * 4 is raw sixteen-bit and never looks anything up. */
    input logic [15:0] pal_ptr,
    input logic pal_xram,
    input logic [2:0] bpp_log,
    input logic reversed,

    /* Segment counts must sum to cw exactly. */
    input logic seg_valid,
    input logic seg_imm,
    input logic [22:0] seg_bits,   /* xram: origin, in bits */
    input logic [7:0] seg_ibits,   /* immediate: the row, MSB first */
    input logic [15:0] seg_fg,     /* immediate: bit set */
    input logic [15:0] seg_bg,     /* immediate: bit clear */
    input logic [9:0] seg_px,
    output logic pixtail_seg_take,

    output logic pixtail_a_req,
    output logic [13:0] pixtail_a_addr,
    input logic a_gnt,
    input logic a_rdy,             /* the grant's word is on a_rdata */
    input logic [31:0] a_rdata,

    output logic pixtail_pal_ld,
    output logic [7:0] pixtail_pal_w,
    output logic [8:0] pixtail_pal_words,
    output logic [7:0] pixtail_pal_idx,
    output logic pixtail_pal_xram,
    output logic pixtail_pal_one_bpp,
    input logic [15:0] pal_q,

    output logic pixtail_px_we,
    output logic [9:0] pixtail_px_addr,
    output logic [15:0] pixtail_px_data,

    output logic pixtail_done
);

    typedef enum logic [1:0] {
        T_IDLE, T_PAL, T_RUN
    } state_t;
    state_t state;

    /* Snapshotted before the first pixel, so the load is
     * content-independent — the fill modes' determinism contract. */
    logic [8:0] pal_words;
    always_comb pal_words = 9'd1 << ((5'd1 << bpp_log) - 5'd1);
    logic [8:0] pal_fetch;
    always_comb pal_fetch = pal_words + {8'd0, pal_ptr[1]};
    logic [8:0] pal_n;
    logic [7:0] pal_w;
    logic pal_skip;
    always_comb pal_skip = !pal_xram || bpp_log == 3'd4;

    /* The fetch side runs ahead into the on-deck segment, so an xram
     * handover costs no bubble. */
    typedef struct packed {
        logic imm;
        logic [22:0] bits;
        logic [7:0] ibits;
        logic [15:0] fg;
        logic [15:0] bg;
        logic [9:0] px;
    } seg_t;
    seg_t cur, deck;
    logic cur_v, deck_v;
    /* No take on a promote edge: a segment offered exactly as cur
     * finishes with the deck empty lands in the deck while the promote
     * copies the deck's old emptiness over it — taken, never emitted,
     * and the line comes up short. */
    always_comb pixtail_seg_take = state == T_RUN && seg_valid
        && (!cur_v || !deck_v) && !cur_done;

    /* Fetch state follows the segment the FETCHER is in, which may be
     * the deck rather than the one being emitted. */
    logic [31:0] fifo[2];
    logic [1:0] fifo_v;
    /* Only a segment's first word carries a bit offset. */
    logic [4:0] fifo_bit0[2];
    logic fifo_seg1[2];            /* word belongs to the deck segment */
    logic [1:0] inflight;
    logic inflight_seg1[2];
    logic [4:0] inflight_bit0[2];
    logic [13:0] fetch_word;
    logic [9:0] fetch_px_left;     /* pixels the fetcher still owes */
    logic fetch_seg1;              /* fetcher is filling the deck */
    logic [4:0] fetch_bit0_next;
    /* Aiming is a standing rule, not a take-time event: a deck taken
     * while the fetcher is busy must still get its turn. */
    logic cur_fetched, deck_fetched;
    logic gnt_q;

    logic [5:0] px_per_word_from;
    always_comb px_per_word_from =
        6'((6'd32 - 6'(fetch_bit0_next)) >> bpp_log);

    /* Combinational so the promote below sees an aim firing on its own
     * edge. The registered copy reads a cycle stale there, and a promote
     * that misses the deck's aim replays the finished segment's words. */
    logic aim_free, aim_cur_now, aim_deck_now;
    always_comb begin
        aim_free = state == T_RUN && fetch_px_left == 10'd0
            && inflight == 2'd0;
        aim_cur_now = aim_free && cur_v && !cur_fetched;
        aim_deck_now = aim_free && !aim_cur_now && deck_v && !deck_fetched;
    end

    logic [4:0] bit_in_word;
    logic [7:0] cur_byte;
    always_comb cur_byte = fifo[0][{bit_in_word[4:3], 3'b000}+:8];
    logic [7:0] pix_idx;
    always_comb begin
        case (bpp_log)
            3'd0: pix_idx = {7'd0, reversed
                ? cur_byte[bit_in_word[2:0]]
                : cur_byte[3'd7 - bit_in_word[2:0]]};
            3'd1: pix_idx = {6'd0, reversed
                ? cur_byte[{bit_in_word[2:1], 1'b0}+:2]
                : cur_byte[{2'd3 - bit_in_word[2:1], 1'b0}+:2]};
            3'd2: pix_idx = {4'd0, reversed
                ? cur_byte[{bit_in_word[2], 2'b00}+:4]
                : cur_byte[{!bit_in_word[2], 2'b00}+:4]};
            default: pix_idx = cur_byte;
        endcase
    end
    logic [15:0] pix16;
    always_comb pix16 = bit_in_word[4] ? fifo[0][31:16] : fifo[0][15:0];

    /* Immediate segments slice their own byte MSB first and pick between
     * two finished colours: mode 1's font mux, generalized. */
    logic [2:0] imm_bit;
    logic imm_on;
    always_comb imm_on = cur.ibits[3'd7 - imm_bit];

    always_comb begin
        pixtail_pal_ld = !abort_i && !start && state == T_PAL
            && !pal_skip && gnt_q;
        pixtail_pal_w = pal_w;
        pixtail_pal_words = pal_words;
        pixtail_pal_idx = pix_idx;
        pixtail_pal_xram = pal_xram;
        pixtail_pal_one_bpp = bpp_log == 3'd0;
    end

    logic [9:0] px;
    logic [9:0] cur_left;
    logic emit_imm, emit_xram, emit_now;
    always_comb begin
        emit_imm = state == T_RUN && cur_v && cur.imm;
        emit_xram = state == T_RUN && cur_v && !cur.imm && fifo_v[0]
            && !fifo_seg1[0];
        emit_now = emit_imm || emit_xram;
    end

    logic word_last;
    always_comb word_last = emit_xram
        && (cur_left == 10'd1
            || 6'(bit_in_word) + {1'b0, 5'd1 << bpp_log} == 6'd32);

    always_comb begin
        pixtail_px_we = emit_now;
        pixtail_px_addr = px;
        pixtail_px_data = 16'h0000;
        if (emit_imm)
            pixtail_px_data = imm_on ? cur.fg : cur.bg;
        else if (emit_xram)
            pixtail_px_data = bpp_log == 3'd4 ? pix16 : pal_q;
    end

    /* The address is combinational, so back-to-back grants take the live
     * counter and never re-read a word. */
    always_comb begin
        pixtail_a_req = 1'b0;
        pixtail_a_addr = fetch_word;
        case (state)
            T_PAL: begin
                pixtail_a_req = !pal_skip && pal_n < pal_fetch;
                pixtail_a_addr = pal_ptr[15:2] + {5'd0, pal_n};
            end
            T_RUN: pixtail_a_req = fetch_px_left != 10'd0
                && 3'(fifo_v) + 3'(inflight) < 3'd2;
            default: ;
        endcase
    end

    initial begin
        state = T_IDLE;
        pal_n = '0;
        pal_w = '0;
        cur_v = 1'b0;
        deck_v = 1'b0;
        cur = '0;
        deck = '0;
        fifo_v = '0;
        inflight = '0;
        fetch_word = '0;
        fetch_px_left = '0;
        fetch_seg1 = 1'b0;
        fetch_bit0_next = '0;
        cur_fetched = 1'b0;
        deck_fetched = 1'b0;
        gnt_q = 1'b0;
        bit_in_word = '0;
        imm_bit = '0;
        px = '0;
        cur_left = '0;
        pixtail_done = 1'b0;
        for (int i = 0; i < 2; i++) begin
            fifo[i] = '0;
            fifo_bit0[i] = '0;
            fifo_seg1[i] = 1'b0;
            inflight_seg1[i] = 1'b0;
            inflight_bit0[i] = '0;
        end
    end
    always_ff @(posedge clk) begin
        gnt_q <= a_gnt;
        pixtail_done <= 1'b0;
        if (abort_i) begin
`ifdef VERILATOR
            if (state != T_IDLE)
                $fatal(1, "pixtail underrun");
`endif
            state <= T_IDLE;
        end else if (start) begin
            pal_n <= '0;
            pal_w <= '0;
            cur_v <= 1'b0;
            deck_v <= 1'b0;
            cur_fetched <= 1'b0;
            deck_fetched <= 1'b0;
            fifo_v <= '0;
            inflight <= '0;
            fetch_px_left <= '0;
            px <= '0;
            state <= T_PAL;
        end else begin
            case (state)
                T_IDLE: ;
                T_PAL: begin
                    if (pal_skip) begin
                        state <= T_RUN;
                    end else begin
                        if (a_gnt)
                            pal_n <= pal_n + 9'd1;
                        if (gnt_q) begin
                            pal_w <= pal_w + 8'd1;
                            if ({1'b0, pal_w} == pal_fetch - 9'd1) begin
                                pal_w <= '0;
                                state <= T_RUN;
                            end
                        end
                    end
                end
                T_RUN: begin
                    if (pixtail_seg_take) begin
                        if (!cur_v) begin
                            cur.imm <= seg_imm;
                            cur.bits <= seg_bits;
                            cur.ibits <= seg_ibits;
                            cur.fg <= seg_fg;
                            cur.bg <= seg_bg;
                            cur.px <= seg_px;
                            cur_v <= 1'b1;
                            cur_left <= seg_px;
                            cur_fetched <= seg_imm;
                            imm_bit <= '0;
                        end else begin
                            deck.imm <= seg_imm;
                            deck.bits <= seg_bits;
                            deck.ibits <= seg_ibits;
                            deck.fg <= seg_fg;
                            deck.bg <= seg_bg;
                            deck.px <= seg_px;
                            deck_v <= 1'b1;
                            deck_fetched <= seg_imm;
                        end
                    end

                    /* Take order is fetch order, so cur outranks the
                     * deck. */
                    if (aim_cur_now) begin
                        fetch_word <= 14'(cur.bits >> 5);
                        fetch_bit0_next <= 5'(cur.bits & 23'd31);
                        fetch_px_left <= cur.px;
                        fetch_seg1 <= 1'b0;
                        cur_fetched <= 1'b1;
                    end else if (aim_deck_now) begin
                        fetch_word <= 14'(deck.bits >> 5);
                        fetch_bit0_next <= 5'(deck.bits & 23'd31);
                        fetch_px_left <= deck.px;
                        fetch_seg1 <= 1'b1;
                        deck_fetched <= 1'b1;
                    end

                    if (a_gnt && state == T_RUN) begin
                        fetch_word <= fetch_word + 14'd1;
                        fetch_px_left <= fetch_px_left
                            < {4'd0, px_per_word_from}
                            ? 10'd0
                            : fetch_px_left - {4'd0, px_per_word_from};
                        fetch_bit0_next <= '0;
                    end

                    /* One rule keeps the slicer honest: whenever
                     * fifo[0] receives a word, bit_in_word loads that
                     * word's tag. Only a first word carries an
                     * offset, so promotion needs no special case. */
                    if (word_shift) begin
                        if (fifo_v[1]) begin
                            fifo[0] <= fifo[1];
                            fifo_bit0[0] <= fifo_bit0[1];
                            fifo_seg1[0] <= fifo_seg1[1] && !cur_done;
                            bit_in_word <= fifo_bit0[1];
                            if (gnt_q) begin
                                fifo[1] <= a_rdata;
                                fifo_bit0[1] <= inflight_bit0[0];
                                fifo_seg1[1] <= inflight_seg1[0]
                                    && !cur_done;
                            end
                            fifo_v <= {gnt_q, 1'b1};
                        end else begin
                            if (gnt_q) begin
                                fifo[0] <= a_rdata;
                                fifo_bit0[0] <= inflight_bit0[0];
                                fifo_seg1[0] <= inflight_seg1[0]
                                    && !cur_done;
                                bit_in_word <= inflight_bit0[0];
                            end
                            fifo_v <= {1'b0, gnt_q};
                        end
                    end else if (gnt_q) begin
                        if (!fifo_v[0]) begin
                            fifo[0] <= a_rdata;
                            fifo_bit0[0] <= inflight_bit0[0];
                            fifo_seg1[0] <= inflight_seg1[0]
                                && !cur_done;
                            bit_in_word <= inflight_bit0[0];
                        end else begin
                            fifo[1] <= a_rdata;
                            fifo_bit0[1] <= inflight_bit0[0];
                            fifo_seg1[1] <= inflight_seg1[0]
                                && !cur_done;
                        end
                        fifo_v <= {fifo_v[0], 1'b1};
                    end

                    inflight <= inflight + (a_gnt ? 2'd1 : 2'd0)
                        - (gnt_q ? 2'd1 : 2'd0);
                    if (gnt_q) begin
                        inflight_seg1[0] <= inflight_seg1[1];
                        inflight_bit0[0] <= inflight_bit0[1];
                        if (a_gnt) begin
                            inflight_seg1[1] <= fetch_seg1;
                            inflight_bit0[1] <= fetch_bit0_next;
                        end
                    end else if (a_gnt) begin
                        inflight_seg1[inflight[0]] <= fetch_seg1;
                        inflight_bit0[inflight[0]] <= fetch_bit0_next;
                    end

                    if (emit_now) begin
                        px <= px + 10'd1;
                        cur_left <= cur_left - 10'd1;
                        if (emit_imm)
                            imm_bit <= imm_bit + 3'd1;
                        else if (!word_shift)
                            bit_in_word <= bit_in_word
                                + (5'd1 << bpp_log);
                        if (cur_done) begin
                            /* The deck's words are already arriving
                             * behind cur's, so every deck tag becomes
                             * a cur tag on the promote. */
                            cur <= deck;
                            cur_v <= deck_v;
                            cur_left <= deck.px;
                            cur_fetched <= deck_fetched
                                || aim_deck_now;
                            deck_v <= 1'b0;
                            deck_fetched <= 1'b0;
                            imm_bit <= '0;
                            for (int i = 0; i < 2; i++) begin
                                fifo_seg1[i] <= 1'b0;
                                inflight_seg1[i] <= 1'b0;
                            end
                            fetch_seg1 <= 1'b0;
                        end
                        if (px == cw - 10'd1) begin
                            state <= T_IDLE;
                            cur_v <= 1'b0;
                            deck_v <= 1'b0;
                            pixtail_done <= 1'b1;
                        end
                    end
                end
                default: state <= T_IDLE;
            endcase
        end
    end

    logic cur_done;
    always_comb cur_done = emit_now && cur_left == 10'd1;
    logic word_shift;
    always_comb word_shift = word_last;

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pixtail;
    always_comb unused_pixtail = ^{a_rdy, cur.bits, cur.px,
                                       pal_ptr[0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
