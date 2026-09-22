/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 5, the paletted sprites of core/vga/mode/mode5.c: an array of
 * descriptors — position, image, palette — walked in order so later
 * sprites land on earlier ones. A plane's sprites are all one fixed
 * square at 1, 2, 4 or 8 bits per pixel, or each descriptor carries
 * its own size, depth, flips and doubling in two more bytes. The
 * palette is read live per pixel, never snapshotted; a color writes
 * only where its alpha bit is set.
 */

module mode5 (
    input logic clk,

    input logic start,
    input logic abort_i,
    input logic [15:0] attr,
    input logic [15:0] cfg,
    input logic [15:0] length,
    input logic [8:0] t_row,
    input logic [9:0] cw,

    output logic mode5_a_req,
    output logic [13:0] mode5_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    /* On a miss the cache fills through this engine's own channel while
     * the pixel stalls. The two never request together: a palette lookup
     * only exists while the index word is already in hand. */
    output logic mode5_pal_lookup,
    output logic mode5_pal_xram,
    output logic mode5_pal_one_bpp,
    output logic [15:0] mode5_pal_base,
    output logic [7:0] mode5_pal_idx,
    output logic [7:0] mode5_pal_idx_b,
    output logic mode5_pal_need_b,
    input logic pal_hit,
    input logic [15:0] pal_qa,
    input logic [15:0] pal_qb,

    /* Two pixels a clock: mode5_px_data[15:0] at mode5_px_addr under
     * mode5_px_we[0], [31:16] at the next pixel under [1]. */
    output logic [1:0] mode5_px_we,
    output logic [9:0] mode5_px_addr,
    output logic [31:0] mode5_px_data,

    output logic mode5_done
);

    /* attr[5:3] the square's size, or 7 for the custom descriptor that
     * carries its own; attr[1:0] the fixed depth. Taken once at the
     * start rather than re-derived, so the plane's slot mux does not
     * stand in front of every descriptor's decode. */
    logic custom_w;
    always_comb custom_w = attr[5:3] == 3'd7;
    logic custom;
    logic [7:0] n4_attr;    /* the fixed size in 4-pixel units */
    logic [1:0] bpp_attr;

    typedef enum logic [2:0] {
        M5_IDLE, M5_DECODE, M5_CLIP, M5_JUDGE, M5_PIX
    } state_t;
    state_t state;

    logic [15:0] idx;

    /* XRAM answers the sprite stage two clocks after a grant, and each
     * grant is remembered as the list's or the row's so the word goes
     * to the right queue. */
    logic gnt_d1, gnt_d, tag1, tag2;

    /* The list streams in ahead of the sprites through its own queue,
     * a word asked for whenever the row leaves the slot free and the
     * queue and the two clocks in flight can take one, up to the last
     * descriptor's word. A descriptor is read out of the queue's first
     * words and popped whole, so a list on a halfword boundary differs
     * only in where the window starts. A custom descriptor is ten bytes,
     * so its window alternates halves and its pop is two words or
     * three. */
    logic [31:0] dq[5];
    logic [2:0] dqn;
    logic [13:0] dfp, dend;
    logic half;             /* the next descriptor starts mid-word */
    logic [95:0] dwin;
    always_comb dwin = {dq[2], dq[1], dq[0]};
    logic [79:0] dsc;
    always_comb dsc = half ? dwin[95:16] : dwin[79:0];
    logic dsc_v;
    always_comb dsc_v = dqn >= ((custom || half) ? 3'd3 : 3'd2);
    logic [2:0] pop_n;
    always_comb pop_n = (custom && half) ? 3'd3 : 3'd2;
    logic [16:0] list_end;
    always_comb list_end = {1'b0, cfg}
        + (custom_w ? {1'b0, length[12:0], 3'b000}
                          + {3'b000, length[12:0], 1'b0}
                    : {1'b0, length[12:0], 3'b000})
        - 17'd1;

    /* The descriptor's size and depth, or the plane's. */
    logic [7:0] dec_n4w, dec_n4h;
    logic [1:0] dec_bpp;
    always_comb begin
        dec_n4w = custom ? {3'd0, 5'({1'b0, dsc[67:64]} + 5'd1)} : n4_attr;
        dec_n4h = custom ? {3'd0, 5'({1'b0, dsc[71:68]} + 5'd1)} : n4_attr;
        dec_bpp = custom ? dsc[73:72] : bpp_attr;
    end

    /* Registered from the window, which moves on to the next descriptor
     * while this one draws. */
    logic signed [15:0] d_x, d_y;
    logic [15:0] d_sptr, d_pptr;
    logic [9:0] d_w, d_h;
    logic [1:0] bpp_log;
    logic [9:0] bytes_per_row;  /* a row starts on a byte */
    logic d_hflip, d_vflip, d_hdbl, d_vdbl;
    logic [16:0] data_size;

    logic [15:0] tex_y;
    always_comb tex_y = {7'd0, t_row} - 16'(d_y);

    /* The clip, from the registers alone: the sprite's footprint on the
     * canvas and the pixels of it cut off at each edge. The walk always
     * ascends the image, so under a flip the right-hand cut is where it
     * starts. */
    logic [10:0] h_dbl;
    always_comb h_dbl = {1'b0, d_h} << d_vdbl;
    logic [9:0] row;
    always_comb row = tex_y[9:0] >> d_vdbl;
    logic [9:0] f_w;
    always_comb f_w = d_w << d_hdbl;
    logic signed [16:0] cw_s;
    always_comb cw_s = 17'($signed({7'd0, cw}));
    logic signed [16:0] x_f, x_fc;
    always_comb x_f = 17'(d_x) + 17'($signed({7'd0, f_w}));
    always_comb x_fc = x_f - cw_s;
    logic [8:0] row_sel;
    logic [9:0] lo, hi, x_top;
    logic [9:0] a_cut, b_cut, a_img, b_img;
    always_comb begin
        a_cut = d_hflip ? hi : lo;
        b_cut = d_hflip ? lo : hi;
        a_img = a_cut >> d_hdbl;
        b_img = b_cut >> d_hdbl;
    end

    logic signed [15:0] px_end;
    logic pal_xram;
    logic [16:0] row_addr;

    /* The row's bytes stream in ahead of the pixels through a short
     * queue: a word is asked for whenever the queue and the two clocks
     * in flight can take it, up to the row's last, so the head of the
     * queue is always the word the pixel is in and a word boundary is
     * a pop. The palette cache's fills share this channel and are told
     * apart by their own grant. */
    logic [31:0] q[4];
    logic [2:0] qn;
    logic [13:0] fp;    /* the next word to ask for, once one has been */
    logic fp_v;
    logic signed [15:0] px_i;   /* pixel within the sprite row */
    logic [9:0] dst;
    /* A doubled pixel half off the canvas at the first or last column. */
    logic lead, tail;

    logic [16:0] pix_byte_addr;
    always_comb pix_byte_addr = row_addr
        + {4'd0, 13'(16'(px_i) << bpp_log) >> 3};
    /* The pixel's byte, stepped with the pixel rather than made from the
     * row's address adder each time, so that adder is not in the path to
     * the palette. It is taken from the adder as the row starts, in the
     * clocks the first word takes to arrive. */
    logic [1:0] pb;
    logic pb_v;
    logic [16:0] end_byte;
    always_comb end_byte = row_addr
        + {4'd0, 13'(16'(px_end) << bpp_log) >> 3};
    logic [13:0] fetch_word;
    always_comb fetch_word = fp_v ? fp : pix_byte_addr[15:2];
    logic fetch_more;
    always_comb fetch_more = !fp_v || fp <= end_byte[15:2];
    logic q_v;
    always_comb q_v = qn != 3'd0;
    /* The pixel and its right-hand neighbour, which is the next index in
     * the byte or the first in the next. The two go out together when the
     * neighbour is in the cached word and inside the sprite; a pair never
     * spans a byte at 1, 2 or 4 bpp once the first pixel is even, and
     * the end of a word only ever strands the last byte's pixels. A
     * doubled sprite goes out a pixel at a time, each as its own pair. */
    logic [3:0] step;
    always_comb step = 4'(4'd1 << bpp_log);
    logic [2:0] bit_off;
    always_comb bit_off = 3'(16'(px_i) << bpp_log);
    logic [3:0] bit_off_j;
    always_comb bit_off_j = {1'b0, bit_off} + step;
    logic adv_byte;
    always_comb adv_byte = bit_off_j[3];
    logic [1:0] byte_j;
    always_comb byte_j = pb + {1'b0, adv_byte};
    logic px_last, px_j_last;
    always_comb px_last = px_i == px_end;
    always_comb px_j_last = px_i + 16'sd1 == px_end;
    logic pair_ok;
    always_comb pair_ok = !px_last && !(adv_byte && pb == 2'b11)
        && !d_hdbl;
    logic two;
    always_comb two = d_hdbl ? !(lead || (px_last && tail)) : pair_ok;
    /* Whether the pixels going out are the cached word's last, so the
     * word read ahead can take its place on the same clock. */
    logic [3:0] bit_off_jj;
    always_comb bit_off_jj = {1'b0, bit_off_j[2:0]} + step;
    logic word_done;
    always_comb word_done = pair_ok
        ? byte_j == 2'b11 && bit_off_jj[3]
        : pb == 2'b11 && adv_byte;

    logic [7:0] cur_byte, cur_byte_j;
    always_comb cur_byte = q[0][{pb, 3'b000}+:8];
    always_comb cur_byte_j = q[0][{byte_j, 3'b000}+:8];
    function automatic logic [7:0] index_of(input logic [7:0] b,
                                            input logic [2:0] off);
        case (bpp_log)
            2'd0: return {7'd0, b[3'd7 - off]};
            2'd1: return {6'd0, b[{2'd3 - off[2:1], 1'b0}+:2]};
            2'd2: return {4'd0, b[{!off[2], 2'b00}+:4]};
            default: return b;
        endcase
    endfunction
    logic [7:0] pix_idx, pix_idx_j;
    always_comb pix_idx = index_of(cur_byte, bit_off);
    always_comb pix_idx_j = index_of(cur_byte_j, bit_off_j[2:0]);
    /* The cache resolves XRAM and builtin palettes alike into a
     * finished color; this engine only names the question. */
    always_comb begin
        mode5_pal_lookup = state == M5_PIX && q_v && pal_xram;
        mode5_pal_xram = pal_xram;
        mode5_pal_one_bpp = bpp_log == 2'd0;
        mode5_pal_base = d_pptr;
        mode5_pal_idx = pix_idx;
        mode5_pal_idx_b = d_hdbl ? pix_idx : pix_idx_j;
        mode5_pal_need_b = two;
    end

    logic emit, pop, dpop;
    always_comb emit = state == M5_PIX && q_v && pb_v && pal_hit;
    always_comb pop = emit && word_done;
    always_comb dpop = state == M5_DECODE && dsc_v;

    /* The row's words come first; the list fills in behind. The row
     * queue's pop this clock is not counted, so it is never asked to
     * hold more than it has room for; the list queue's is, since a
     * descriptor leaves two or three words at once and the list would
     * otherwise wait a clock on every other one. */
    logic pix_land, desc_land, pix_req, desc_req, req_is_desc;
    always_comb begin
        pix_land = gnt_d && !tag2;
        desc_land = gnt_d && tag2;
        pix_req = state == M5_PIX && fetch_more
            && qn + {2'd0, pix_land} + {2'd0, gnt_d1 && !tag1} < 3'd4;
        desc_req = state != M5_IDLE && dfp <= dend
            && dqn + {2'd0, desc_land} + {2'd0, gnt_d1 && tag1}
               - (dpop ? pop_n : 3'd0) < 3'd5;
        req_is_desc = !pix_req && desc_req;
        mode5_a_req = pix_req || desc_req;
        mode5_a_addr = pix_req ? fetch_word : dfp;
    end

    /* The write lands only where the color carries alpha, and only when the
     * cache has answered. A miss stalls the pixel but does not make the scan
     * wrong. Builtin palettes always hit. The colors come a clock after
     * the lookup, so the write is a clock behind the pixel, and a sprite's
     * last pair is written while the next sprite is decoded. A flipped
     * sprite walks the canvas downward, so its pair lands a pixel below
     * dst with the two colors swapped. */
    logic wr_v, wr_pair, wr_flip;
    logic [9:0] wr_dst;
    logic [15:0] lo_c, hi_c;
    always_comb begin
        lo_c = (wr_flip && wr_pair) ? pal_qb : pal_qa;
        hi_c = wr_flip ? pal_qa : pal_qb;
        mode5_px_we = 2'b00;
        mode5_px_addr = wr_dst;
        mode5_px_data = {hi_c, lo_c};
        if (wr_v) begin
            mode5_px_we[0] = lo_c[5];
            mode5_px_we[1] = wr_pair && hi_c[5];
        end
    end

    task automatic next_sprite();
        /* Whatever was read ahead belonged to the sprite just finished. */
        qn <= '0;
        fp_v <= 1'b0;
        pb_v <= 1'b0;
        if (idx + 16'd1 == length) begin
            mode5_done <= 1'b1;
            state <= M5_IDLE;
        end else begin
            idx <= idx + 16'd1;
            state <= M5_DECODE;
        end
    endtask

    task automatic step_pixel();
        lead <= 1'b0;
        if (px_last || (pair_ok && px_j_last))
            next_sprite();
        else begin
            px_i <= px_i + (pair_ok ? 16'sd2 : 16'sd1);
            dst <= d_hflip ? dst - (two ? 10'd2 : 10'd1)
                           : dst + (two ? 10'd2 : 10'd1);
            pb <= pb + {1'b0, adv_byte}
                + {1'b0, pair_ok && bit_off_jj[3]};
        end
    endtask

    initial begin
        state = M5_IDLE;
        idx = '0;
        gnt_d1 = 1'b0;
        gnt_d = 1'b0;
        tag1 = 1'b0;
        tag2 = 1'b0;
        custom = 1'b0;
        n4_attr = '0;
        bpp_attr = '0;
        for (int j = 0; j < 5; j++)
            dq[j] = '0;
        dqn = '0;
        dfp = '0;
        dend = '0;
        half = 1'b0;
        d_x = '0;
        d_y = '0;
        d_sptr = '0;
        d_pptr = '0;
        d_w = '0;
        d_h = '0;
        bpp_log = '0;
        bytes_per_row = '0;
        d_hflip = 1'b0;
        d_vflip = 1'b0;
        d_hdbl = 1'b0;
        d_vdbl = 1'b0;
        data_size = '0;
        row_sel = '0;
        lo = '0;
        hi = '0;
        x_top = '0;
        px_end = '0;
        pal_xram = 1'b0;
        row_addr = '0;
        for (int j = 0; j < 4; j++)
            q[j] = '0;
        qn = '0;
        fp = '0;
        fp_v = 1'b0;
        pb = '0;
        pb_v = 1'b0;
        px_i = '0;
        dst = '0;
        lead = 1'b0;
        tail = 1'b0;
        wr_v = 1'b0;
        wr_pair = 1'b0;
        wr_flip = 1'b0;
        wr_dst = '0;
        mode5_done = 1'b0;
    end
    always_ff @(posedge clk) begin
        gnt_d1 <= a_gnt;
        gnt_d <= gnt_d1;
        tag1 <= req_is_desc;
        tag2 <= tag1;
        wr_v <= emit && !abort_i;
        wr_pair <= two;
        wr_flip <= d_hflip;
        wr_dst <= (d_hflip && two) ? dst - 10'd1 : dst;
        mode5_done <= 1'b0;
        if (abort_i) begin
            /* sprite.sv has already counted this lost line; drop it. */
            state <= M5_IDLE;
        end else if (start) begin
            idx <= '0;
            qn <= '0;
            fp_v <= 1'b0;
            pb_v <= 1'b0;
            dqn <= '0;
            dfp <= cfg[15:2];
            dend <= list_end[15:2];
            half <= cfg[1];
            custom <= custom_w;
            n4_attr <= 8'(8'd2 << attr[5:3]);
            bpp_attr <= attr[1:0];
            if (length == 16'd0) begin
                mode5_done <= 1'b1;
                state <= M5_IDLE;
            end else
                state <= M5_DECODE;
        end else begin
            if (req_is_desc && a_gnt)
                dfp <= dfp + 14'd1;
            if (dpop) begin
                if (pop_n == 3'd3) begin
                    dq[0] <= dq[3];
                    dq[1] <= dq[4];
                end else begin
                    dq[0] <= dq[2];
                    dq[1] <= dq[3];
                    dq[2] <= dq[4];
                end
            end
            if (desc_land && state != M5_IDLE)
                dq[dpop ? dqn - pop_n : dqn] <= a_rdata;
            dqn <= dqn + {2'd0, desc_land && state != M5_IDLE}
                - (dpop ? pop_n : 3'd0);
            case (state)
                M5_IDLE: ;
                M5_DECODE:
                    if (dsc_v) begin
                        d_x <= dsc[15:0];
                        d_y <= dsc[31:16];
                        d_sptr <= dsc[47:32];
                        d_pptr <= dsc[63:48];
                        d_w <= {dec_n4w, 2'b00};
                        d_h <= {dec_n4h, 2'b00};
                        bpp_log <= dec_bpp;
                        bytes_per_row <= 10'((12'({4'd0, dec_n4w} << dec_bpp)
                                              + 12'd1) >> 1);
                        d_hflip <= custom && dsc[76];
                        d_vflip <= custom && dsc[77];
                        d_hdbl <= custom && dsc[78];
                        d_vdbl <= custom && dsc[79];
                        if (custom)
                            half <= !half;
                        state <= M5_CLIP;
                    end
                M5_CLIP: begin
                    row_sel <= d_vflip ? 9'(d_h - 10'd1 - row) : row[8:0];
                    lo <= d_x < 0 ? 10'(-d_x) : 10'd0;
                    hi <= x_fc > 17'sd0 ? 10'(x_fc) : 10'd0;
                    x_top <= x_fc > 17'sd0 ? cw - 10'd1 : 10'(x_f - 17'sd1);
                    data_size <= 17'(17'({7'd0, d_h})
                                     * 17'({7'd0, bytes_per_row}));
                    if (tex_y >= {5'd0, h_dbl}
                        || x_f <= 17'sd0
                        || 17'(d_x) >= cw_s)
                        next_sprite();
                    else
                        state <= M5_JUDGE;
                end
                M5_JUDGE: begin
                    px_i <= $signed({6'd0, a_img});
                    px_end <= $signed({6'd0, d_w}) - 16'sd1
                        - $signed({6'd0, b_img});
                    dst <= d_hflip ? x_top : (d_x < 0 ? 10'd0 : d_x[9:0]);
                    lead <= d_hdbl && a_cut[0];
                    tail <= d_hdbl && b_cut[0];
                    pal_xram <= !d_pptr[0]
                        && {1'b0, d_pptr}
                            <= 17'h10000
                                - (17'd2 << {12'd0, 5'd1 << bpp_log});
                    row_addr <= {1'b0, d_sptr}
                        + 17'(17'({8'd0, row_sel})
                              * 17'({7'd0, bytes_per_row}));
                    if ({1'b0, d_sptr} > 17'h10000 - data_size)
                        next_sprite();
                    else
                        state <= M5_PIX;
                end
                M5_PIX: begin
                    if (!pb_v) begin
                        pb <= pix_byte_addr[1:0];
                        pb_v <= 1'b1;
                    end
                    if (pix_req && a_gnt) begin
                        fp <= fetch_word + 14'd1;
                        fp_v <= 1'b1;
                    end
                    if (pop) begin
                        q[0] <= q[1];
                        q[1] <= q[2];
                        q[2] <= q[3];
                    end
                    if (pix_land)
                        q[2'(pop ? qn - 3'd1 : qn)] <= a_rdata;
                    qn <= qn + {2'd0, pix_land} - {2'd0, pop};
                    if (emit)
                        step_pixel();
                    /* else: the cache is filling on this channel, or
                     * the word is still on its way. */
                end
                default: state <= M5_IDLE;
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_mode5;
    always_comb unused_mode5 = ^{attr[15:6], attr[2], dsc[75:74],
                                     list_end[16], list_end[1:0],
                                     length[15:13],
                                     pix_byte_addr[16], bit_off_jj[2:0],
                                     end_byte[16], end_byte[1:0],
                                     idx[15:13]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
