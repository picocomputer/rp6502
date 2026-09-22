/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pixel tail every fill mode shares, so that the pixel-exact detail
 * exists once. A line is a sequence of segments. An xram segment is a bit
 * origin in XRAM and a pixel count; an immediate segment is eight bits
 * and the two finished colors those bits choose between.
 *
 * A font row is a 1bpp bitmap and a cell's foreground and background are
 * a two-entry palette, so mode 1 is one immediate segment a cell; a mode
 * 3 wraparound ends one segment and starts the next; transparent padding
 * is an immediate segment of zeros, so blank and out-of-window lines need
 * no special case.
 *
 * Up to two pixels leave a clock, neighbours, and linebuf.sv lands the
 * pair in one write. The palette answers both through its two ports.
 */

module pixtail
    import vid_palette_pkg::*;
(
    input logic clk,

    input logic start,
    input logic abort_i,
    input logic [9:0] cw,

    /* The palette plan. None of it is latched here, so it has to hold
     * from start until done. bpp_log 0 through 3 index the palette; 4 is
     * raw sixteen-bit color and looks nothing up. */
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
    input logic a_rdy,
    input logic [31:0] a_rdata,

    output logic pixtail_pal_ld,
    output logic [7:0] pixtail_pal_w,
    output logic [8:0] pixtail_pal_words,
    output logic [7:0] pixtail_pal_idx,
    output logic [7:0] pixtail_pal_idx1,
    output logic pixtail_pal_xram,
    output logic pixtail_pal_one_bpp,
    input logic [15:0] pal_q,
    input logic [15:0] pal_q1,

    /* px_data[15:0] lands at px_addr under px_we[0], px_data[31:16] at
     * px_addr + 1 under px_we[1]. */
    output logic [1:0] pixtail_px_we,
    output logic [9:0] pixtail_px_addr,
    output logic [31:0] pixtail_px_data,

    output logic pixtail_done
);

    typedef enum logic [1:0] {
        T_IDLE, T_PAL, T_RUN
    } state_t;
    state_t state;

    /* The whole palette is fetched before the first pixel, and how much
     * is fetched depends only on bpp_log and the palette's halfword
     * alignment, never on the image. The fill modes are contracted to be
     * deterministic that way. */
    logic [8:0] pal_words;
    always_comb pal_words = 9'd1 << ((5'd1 << bpp_log) - 5'd1);
    logic [8:0] pal_fetch;
    always_comb pal_fetch = pal_words + {8'd0, pal_ptr[1]};
    logic [8:0] pal_n;
    logic [7:0] pal_w;
    logic pal_skip;
    always_comb pal_skip = !pal_xram || bpp_log == 3'd4;

    /* The fetch side runs ahead into the on-deck segment, so handing over
     * from one xram segment to the next costs no bubble. */
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
    /* No take on a promote clock: a segment offered exactly as cur
     * finishes, with the deck empty, would be written into the deck while
     * the promote below writes the deck's old emptiness over it. It would
     * be taken and never emitted, and the line would come up short. */
    always_comb pixtail_seg_take = state == T_RUN && seg_valid
        && (!cur_v || !deck_v) && !cur_done;

    /* The fetch bookkeeping follows the segment being fetched, which may
     * be the deck rather than the one being emitted. */
    /* Three words deep, because sixteen-bit color is the one depth whose
     * pixel pair is a whole word: to land a pair every clock it needs one
     * word emitting, one ready behind it and one still in flight. Every
     * narrower depth spends several clocks on a word and never fills it. */
    logic [31:0] fifo[3];
    logic [2:0] fifo_v;
    /* Only a segment's first word carries a bit offset. */
    logic [4:0] fifo_bit0[3];
    logic fifo_seg1[3];            /* word is part of the deck segment */
    logic [1:0] inflight;
    logic inflight_seg1[2];
    logic [4:0] inflight_bit0[2];
    logic [13:0] fetch_word;
    logic [9:0] fetch_px_left;
    logic fetch_seg1;              /* fetcher is filling the deck */
    logic [4:0] fetch_bit0_next;
    /* Where a segment's later words start, which is its origin modulo the
     * pixel, and zero for every depth that divides a byte. It rides beside
     * fetch_bit0_next rather than replacing the zero it falls to, because
     * that zero is also what makes px_per_word_from count the pixel a word
     * finishes for the word behind it. */
    logic [4:0] fetch_phase;
    /* Aiming the fetcher is a standing condition rather than something
     * that happens when a segment is taken, because a deck segment taken
     * while the fetcher is busy still has to get its turn. */
    logic cur_fetched, deck_fetched;
    logic gnt_q1, gnt_q;

    logic inflight_at;
    always_comb inflight_at = 1'(inflight - (gnt_q ? 2'd1 : 2'd0));

    logic [5:0] px_per_word_from;
    always_comb px_per_word_from =
        6'((6'd32 - 6'(fetch_bit0_next)) >> bpp_log);

    /* These are combinational so that the promote below sees an aim
     * firing on the same edge. A registered copy would read a clock stale
     * there, and a promote that misses the deck's aim replays the
     * finished segment's words. */
    logic aim_free, aim_cur_now, aim_deck_now;
    always_comb begin
        aim_free = state == T_RUN && fetch_px_left == 10'd0
            && inflight == 2'd0;
        aim_cur_now = aim_free && cur_v && !cur_fetched;
        aim_deck_now = aim_free && !aim_cur_now && deck_v && !deck_fetched;
    end

    logic [4:0] bit_in_word;
    /* The second pixel of a pair starts a pixel on from the first, which
     * can be in the word behind fifo[0]; bit_end is where the pixel after
     * the pair would start. */
    logic [5:0] bit_next, bit_end;
    always_comb bit_next = 6'(bit_in_word) + {1'b0, 5'd1 << bpp_log};
    always_comb bit_end = 6'(bit_in_word) + {5'd1 << bpp_log, 1'b0};
    logic [63:0] fifo_pair;
    always_comb fifo_pair = {fifo[1], fifo[0]};
    logic [7:0] cur_byte, byte_1;
    always_comb cur_byte = fifo[0][{bit_in_word[4:3], 3'b000}+:8];
    always_comb byte_1 = fifo_pair[{bit_next[5:3], 3'b000}+:8];
    function automatic logic [7:0] sub_idx(input logic [7:0] b,
                                           input logic [2:0] at,
                                           input logic [2:0] depth,
                                           input logic rev);
        case (depth)
            3'd0: sub_idx = {7'd0, rev ? b[at] : b[3'd7 - at]};
            3'd1: sub_idx = {6'd0, rev ? b[{at[2:1], 1'b0}+:2]
                                     : b[{2'd3 - at[2:1], 1'b0}+:2]};
            3'd2: sub_idx = {4'd0, rev ? b[{at[2], 2'b00}+:4]
                                     : b[{!at[2], 2'b00}+:4]};
            default: sub_idx = b;
        endcase
    endfunction
    logic [7:0] pix_idx, pix_idx1;
    always_comb pix_idx = sub_idx(cur_byte, bit_in_word[2:0], bpp_log,
                                  reversed);
    always_comb pix_idx1 = sub_idx(byte_1, bit_next[2:0], bpp_log,
                                   reversed);
    /* Sixteen-bit color at any byte, which is the only pixel wide enough
     * to reach past its word: byte 3 takes its high half from the word
     * behind it. Everything narrower divides eight and cannot straddle. */
    logic [15:0] pix16, pix16_1;
    always_comb pix16 = 16'(fifo_pair >> {bit_in_word[4:3], 3'b000});
    always_comb pix16_1 = 16'(fifo_pair >> {bit_next[5:3], 3'b000});
    logic straddle;
    always_comb straddle = bit_next > 6'd32;

    logic [2:0] imm_bit;
    logic imm_on, imm_on1;
    always_comb imm_on = cur.ibits[3'd7 - imm_bit];
    always_comb imm_on1 = cur.ibits[3'd6 - imm_bit];

    always_comb begin
        pixtail_pal_ld = !abort_i && !start && state == T_PAL
            && !pal_skip && gnt_q;
        pixtail_pal_w = pal_w;
        pixtail_pal_words = pal_words;
        pixtail_pal_idx = pix_idx;
        pixtail_pal_idx1 = pix_idx1;
        pixtail_pal_xram = pal_xram;
        pixtail_pal_one_bpp = bpp_log == 3'd0;
    end

    logic [9:0] px;
    logic [9:0] cur_left;
    logic emit_imm, emit_xram, emit_now, emit_pair;
    always_comb begin
        emit_imm = state == T_RUN && cur_v && cur.imm;
        emit_xram = state == T_RUN && cur_v && !cur.imm && fifo_v[0]
            && !fifo_seg1[0] && (!straddle || fifo_v[1]);
        emit_now = emit_imm || emit_xram;
        /* The second goes too when the segment has one and, for an xram
         * segment, the fifo holds every bit of it. */
        emit_pair = emit_now && cur_left != 10'd1
            && (emit_imm || bit_end <= 6'd32 || fifo_v[1]);
    end
    logic [5:0] bit_after;
    always_comb bit_after = emit_pair ? bit_end : bit_next;
    logic [9:0] px_after;
    always_comb px_after = px + 10'd1 + {9'd0, emit_pair};

    logic word_last;
    always_comb word_last = emit_xram && (cur_done || bit_after >= 6'd32);

    always_comb begin
        pixtail_px_we = {emit_pair, emit_now};
        pixtail_px_addr = px;
        pixtail_px_data = 32'h0000_0000;
        if (emit_imm)
            pixtail_px_data = {imm_on1 ? cur.fg : cur.bg,
                               imm_on ? cur.fg : cur.bg};
        else if (emit_xram)
            pixtail_px_data = bpp_log == 3'd4 ? {pix16_1, pix16}
                                              : {pal_q1, pal_q};
    end

    always_comb begin
        pixtail_a_req = 1'b0;
        pixtail_a_addr = fetch_word;
        case (state)
            T_PAL: begin
                pixtail_a_req = !pal_skip && pal_n < pal_fetch;
                pixtail_a_addr = pal_ptr[15:2] + {5'd0, pal_n};
            end
            /* Three words in the system at most, counting the one that
             * lands and the ones that go this clock, which at two clocks
             * of latency is what keeps a word a clock coming. */
            T_RUN: pixtail_a_req = fetch_px_left != 10'd0
                && (inflight < 2'd2 || gnt_q)
                && 3'(fifo_n) - 3'(drop) + 3'(inflight) < 3'd3;
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
        fetch_phase = '0;
        cur_fetched = 1'b0;
        deck_fetched = 1'b0;
        gnt_q1 = 1'b0;
        gnt_q = 1'b0;
        bit_in_word = '0;
        imm_bit = '0;
        px = '0;
        cur_left = '0;
        pixtail_done = 1'b0;
        for (int i = 0; i < 3; i++) begin
            fifo[i] = '0;
            fifo_bit0[i] = '0;
            fifo_seg1[i] = 1'b0;
        end
        for (int i = 0; i < 2; i++) begin
            inflight_seg1[i] = 1'b0;
            inflight_bit0[i] = '0;
        end
    end
    always_ff @(posedge clk) begin
        gnt_q1 <= a_gnt;
        gnt_q <= gnt_q1;
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
                        fetch_phase <= 5'(cur.bits & 23'd31)
                            & 5'((5'd1 << bpp_log) - 5'd1);
                        fetch_px_left <= cur.px;
                        fetch_seg1 <= 1'b0;
                        cur_fetched <= 1'b1;
                    end else if (aim_deck_now) begin
                        fetch_word <= 14'(deck.bits >> 5);
                        fetch_bit0_next <= 5'(deck.bits & 23'd31);
                        fetch_phase <= 5'(deck.bits & 23'd31)
                            & 5'((5'd1 << bpp_log) - 5'd1);
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

                    for (int i = 0; i < 3; i++) begin
                        fifo[i] <= fifo_nx[i];
                        fifo_bit0[i] <= fifo_bit0_nx[i];
                        fifo_seg1[i] <= fifo_seg1_nx[i];
                    end
                    fifo_v <= fifo_v_nx;
                    /* Whenever fifo[0] receives a word, bit_in_word
                     * loads that word's own offset, or the offset a pixel
                     * straddling into it carried. Only a segment's first
                     * word has a nonzero one of its own, so promotion
                     * needs no special case. */
                    if (fifo_v_nx[0] && (word_shift || !fifo_v[0]))
                        bit_in_word <= word_shift && bit_after > 6'd32
                            && !cur_done
                            ? bit_after[4:0] : fifo_bit0_nx[0];

                    inflight <= inflight + (a_gnt ? 2'd1 : 2'd0)
                        - (gnt_q ? 2'd1 : 2'd0);
                    if (gnt_q) begin
                        inflight_seg1[0] <= inflight_seg1[1];
                        inflight_bit0[0] <= inflight_bit0[1];
                    end
                    /* Behind the words still in flight, which is one fewer
                     * where a word returns on this clock; that one has left
                     * the queue and its slot is the new word's. The write
                     * below the shift so the two agree on that slot. */
                    if (a_gnt) begin
                        inflight_seg1[inflight_at] <= fetch_seg1;
                        inflight_bit0[inflight_at] <= fetch_bit0_next
                            | fetch_phase;
                    end

                    if (emit_now) begin
                        px <= px_after;
                        cur_left <= cur_left - 10'd1 - {9'd0, emit_pair};
                        if (emit_imm)
                            imm_bit <= imm_bit + 3'd1 + {2'd0, emit_pair};
                        else if (!word_shift)
                            bit_in_word <= bit_after[4:0];
                        if (cur_done) begin
                            /* The deck's words are already arriving
                             * behind cur's, so every word marked as the
                             * deck's becomes cur's on the promote. */
                            cur <= deck;
                            cur_v <= deck_v;
                            cur_left <= deck.px;
                            cur_fetched <= deck_fetched
                                || aim_deck_now;
                            deck_v <= 1'b0;
                            deck_fetched <= 1'b0;
                            imm_bit <= '0;
                            for (int i = 0; i < 3; i++)
                                fifo_seg1[i] <= 1'b0;
                            for (int i = 0; i < 2; i++)
                                inflight_seg1[i] <= 1'b0;
                            fetch_seg1 <= 1'b0;
                        end
                        if (px_after == cw) begin
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
    always_comb cur_done = emit_now
        && cur_left == 10'd1 + {9'd0, emit_pair};
    logic word_shift;
    always_comb word_shift = word_last;
    /* A segment whose last pixel reaches into the word behind it leaves
     * that word in the fifo as it ends. It is the finished segment's, so
     * the shift drops it rather than handing it to the next. */
    logic keep_1;
    always_comb keep_1 = fifo_v[1] && (fifo_seg1[1] || !cur_done);
    /* How many words leave the head this clock: the one just emitted, and
     * with it the dead word behind it where there is one. */
    logic [1:0] drop;
    always_comb begin
        drop = 2'd0;
        if (word_shift)
            drop = keep_1 || !fifo_v[1] ? 2'd1 : 2'd2;
    end

    logic [1:0] fifo_n;
    always_comb fifo_n = 2'(fifo_v[0]) + 2'(fifo_v[1]) + 2'(fifo_v[2]);
    logic [2:0] fifo_v_sh;
    always_comb fifo_v_sh = fifo_v >> drop;
    /* A word arriving joins the end of what the drop leaves behind. */
    logic [1:0] fifo_at;
    always_comb fifo_at = 2'(fifo_v_sh[0]) + 2'(fifo_v_sh[1])
        + 2'(fifo_v_sh[2]);
    logic [31:0] fifo_nx[3];
    logic [4:0] fifo_bit0_nx[3];
    logic fifo_seg1_nx[3];
    logic [2:0] fifo_v_nx;
    always_comb begin
        for (int i = 0; i < 3; i++) begin
            fifo_nx[i] = fifo[i];
            fifo_bit0_nx[i] = fifo_bit0[i];
            fifo_seg1_nx[i] = fifo_seg1[i] && !cur_done;
            for (int j = 1; j < 3; j++)
                if (j == i + int'(drop)) begin
                    fifo_nx[i] = fifo[j];
                    fifo_bit0_nx[i] = fifo_bit0[j];
                    fifo_seg1_nx[i] = fifo_seg1[j] && !cur_done;
                end
        end
        fifo_v_nx = fifo_v_sh;
        if (gnt_q) begin
            fifo_nx[fifo_at] = a_rdata;
            fifo_bit0_nx[fifo_at] = inflight_bit0[0];
            fifo_seg1_nx[fifo_at] = inflight_seg1[0] && !cur_done;
            fifo_v_nx[fifo_at] = 1'b1;
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pixtail;
    always_comb unused_pixtail = ^{a_rdy, cur.bits, cur.px,
                                       pal_ptr[0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
