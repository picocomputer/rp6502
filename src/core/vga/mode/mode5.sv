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

module mode5
    import mode::*;
(
    input logic clk,

    input logic start,
    input logic abort_i,
    input logic [15:0] attr,
    input logic [8:0] t_row,
    input logic [9:0] cw,

    output logic mode5_a_req,
    output logic [13:0] mode5_a_addr,
    input logic a_gnt,

    /* The descriptor list queue sprite.sv shares between the engines:
     * what it needs from this one, and the descriptor at its head. */
    output logic mode5_lq_active,
    output logic [3:0] mode5_lq_size,
    output logic mode5_lq_pop,
    output logic mode5_lq_gnt,
    input logic lq_req,
    input logic [13:0] lq_addr,
    input logic [79:0] lq_dsc,
    input logic lq_v,
    input logic lq_end,

    /* The row word queue sprite.sv shares between the engines: what it
     * needs from this one, and the words it holds. */
    output logic mode5_rq_run,
    output logic [13:0] mode5_rq_first,
    output logic [13:0] mode5_rq_last,
    output logic mode5_rq_want,
    output logic mode5_rq_pop,
    output logic mode5_rq_pop_room,
    input logic rq_req,
    input logic [13:0] rq_addr,
    input logic [31:0] rq_q0,
    input logic [2:0] rq_n,

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

    typedef enum logic [1:0] {
        M5_IDLE, M5_DECODE, M5_CLIP, M5_PIX
    } state_t;
    state_t state;

    /* The list streams in ahead of the sprites through its own queue. A
     * custom descriptor is ten bytes, so its window alternates halves. */
    always_comb begin
        mode5_lq_active = state != M5_IDLE;
        mode5_lq_size = custom ? 4'd5 : 4'd4;
        mode5_lq_pop = dpop;
        mode5_lq_gnt = req_is_desc && a_gnt;
    end

    /* The descriptor's size and depth, or the plane's. */
    logic [7:0] dec_n4w, dec_n4h;
    logic [1:0] dec_bpp;
    always_comb begin
        dec_n4w = custom ? {3'd0, 5'({1'b0, lq_dsc[67:64]} + 5'd1)} : n4_attr;
        dec_n4h = custom ? {3'd0, 5'({1'b0, lq_dsc[71:68]} + 5'd1)} : n4_attr;
        dec_bpp = custom ? lq_dsc[73:72] : bpp_attr;
    end

    /* Registered from the window, which moves on to the next descriptor
     * while this one draws. */
    logic signed [15:0] d_x, d_y;
    logic [15:0] d_sptr, d_pptr;
    logic [9:0] d_w, d_h;
    logic [1:0] bpp_log;
    logic [9:0] bytes_per_row;  /* a row starts on a byte */
    logic d_hflip, d_vflip, d_hdbl, d_vdbl;

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
    logic [16:0] data_size;
    logic [9:0] a_cut, b_cut;
    logic [8:0] a_img, b_img;
    always_comb begin
        row_sel = d_vflip ? 9'(d_h + ~row) : row[8:0];
        lo = d_x < 0 ? 10'(-d_x) : 10'd0;
        hi = x_fc > 17'sd0 ? 10'(x_fc) : 10'd0;
        x_top = (x_fc > 17'sd0 ? cw : 10'(x_f)) - 10'd1;
        data_size = 17'(17'({7'd0, d_h}) * 17'({7'd0, bytes_per_row}));
        a_cut = d_hflip ? hi : lo;
        b_cut = d_hflip ? lo : hi;
        a_img = 9'(a_cut >> d_hdbl);
        b_img = 9'(b_cut >> d_hdbl);
    end

    logic [8:0] px_end;
    logic pal_xram;
    logic [16:0] row_addr;

    logic [8:0] px_i;   /* pixel within the sprite row */
    logic [9:0] dst;
    /* A doubled pixel half off the canvas at the first or last column. */
    logic lead, tail;

    logic [16:0] pix_byte_addr;
    always_comb pix_byte_addr = row_addr
        + {5'd0, 12'({3'd0, px_i} << bpp_log) >> 3};
    /* The pixel's byte, stepped with the pixel rather than made from the
     * row's address adder each time, so that adder is not in the path to
     * the palette. It is taken from the adder as the row starts, in the
     * clocks the first word takes to arrive. */
    logic [1:0] pb;
    logic [16:0] end_byte;
    always_comb end_byte = row_addr
        + {5'd0, 12'({3'd0, px_end} << bpp_log) >> 3};

    /* The row's bytes stream in ahead of the pixels through the queue.
     * Its pop this clock is not counted, so it is never asked to hold more
     * than it has room for. The palette cache's fills share this channel
     * and are told apart by their own grant. */
    always_comb begin
        mode5_rq_run = state == M5_PIX;
        mode5_rq_first = pix_byte_addr[15:2];
        mode5_rq_last = end_byte[15:2];
        mode5_rq_want = 1'b1;
        mode5_rq_pop = pop;
        mode5_rq_pop_room = 1'b0;
    end
    logic q_v;
    always_comb q_v = rq_n != 3'd0;
    /* The pixel and its right-hand neighbour, which is the next index in
     * the byte or the first in the next. The two go out together when the
     * neighbour is in the cached word and inside the sprite; a pair never
     * spans a byte at 1, 2 or 4 bpp once the first pixel is even, and
     * the end of a word only ever strands the last byte's pixels. A
     * doubled sprite goes out a pixel at a time, each as its own pair. */
    logic [3:0] step;
    always_comb step = 4'(4'd1 << bpp_log);
    logic [2:0] bit_off;
    always_comb bit_off = 3'(px_i << bpp_log);
    logic [3:0] bit_off_j;
    always_comb bit_off_j = {1'b0, bit_off} + step;
    logic adv_byte;
    always_comb adv_byte = bit_off_j[3];
    logic [1:0] byte_j;
    always_comb byte_j = pb + {1'b0, adv_byte};
    logic px_last, px_j_last;
    always_comb px_last = px_i == px_end;
    always_comb px_j_last = px_i + 9'd1 == px_end;
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
    always_comb cur_byte = rq_q0[{pb, 3'b000}+:8];
    always_comb cur_byte_j = rq_q0[{byte_j, 3'b000}+:8];
    logic [7:0] pix_idx, pix_idx_j;
    always_comb pix_idx = sub_idx(cur_byte, bit_off, {1'b0, bpp_log}, 1'b0);
    always_comb pix_idx_j = sub_idx(cur_byte_j, bit_off_j[2:0],
                                    {1'b0, bpp_log}, 1'b0);
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
    always_comb emit = state == M5_PIX && q_v && pal_hit;
    always_comb pop = emit && word_done;
    always_comb dpop = state == M5_DECODE && lq_v;

    /* The row's words come first; the list fills in behind. */
    logic req_is_desc;
    always_comb begin
        req_is_desc = !rq_req && lq_req;
        mode5_a_req = rq_req || lq_req;
        mode5_a_addr = rq_req ? rq_addr : lq_addr;
    end

    /* The write lands only where the color carries alpha, and only when the
     * cache has answered. A miss stalls the pixel but does not make the scan
     * wrong. Builtin palettes always hit. The colors come a clock after
     * the lookup, so the write is a clock behind the pixel, and a sprite's
     * last pair is written while the next sprite is decoded. A flipped
     * sprite walks the canvas downward, so its pair lands a pixel below
     * dst with the two colors swapped. */
    logic wr_v, wr_pair;
    logic [9:0] wr_dst;
    logic [15:0] lo_c, hi_c;
    always_comb begin
        lo_c = (d_hflip && wr_pair) ? pal_qb : pal_qa;
        hi_c = d_hflip ? pal_qa : pal_qb;
        mode5_px_we = 2'b00;
        mode5_px_addr = wr_dst;
        mode5_px_data = {hi_c, lo_c};
        if (wr_v) begin
            mode5_px_we[0] = lo_c[5];
            mode5_px_we[1] = wr_pair && hi_c[5];
        end
    end

    task automatic next_sprite();
        if (lq_end) begin
            mode5_done <= 1'b1;
            state <= M5_IDLE;
        end else
            state <= M5_DECODE;
    endtask

    task automatic step_pixel();
        lead <= 1'b0;
        if (px_last || (pair_ok && px_j_last))
            next_sprite();
        else begin
            px_i <= px_i + (pair_ok ? 9'd2 : 9'd1);
            dst <= d_hflip ? dst - (two ? 10'd2 : 10'd1)
                           : dst + (two ? 10'd2 : 10'd1);
            pb <= pb + {1'b0, adv_byte}
                + {1'b0, pair_ok && bit_off_jj[3]};
        end
    endtask

    initial begin
        state = M5_IDLE;
        custom = 1'b0;
        n4_attr = '0;
        bpp_attr = '0;
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
        px_end = '0;
        pal_xram = 1'b0;
        row_addr = '0;
        pb = '0;
        px_i = '0;
        dst = '0;
        lead = 1'b0;
        tail = 1'b0;
        wr_v = 1'b0;
        wr_pair = 1'b0;
        wr_dst = '0;
        mode5_done = 1'b0;
    end
    always_ff @(posedge clk) begin
        wr_v <= emit && !abort_i;
        wr_pair <= two;
        wr_dst <= (d_hflip && two) ? dst - 10'd1 : dst;
        mode5_done <= 1'b0;
        if (abort_i) begin
            /* sprite.sv has already counted this lost line; drop it. */
            state <= M5_IDLE;
        end else if (start) begin
            custom <= custom_w;
            n4_attr <= 8'(8'd2 << attr[5:3]);
            bpp_attr <= attr[1:0];
            state <= M5_DECODE;
        end else begin
            case (state)
                M5_IDLE: ;
                M5_DECODE:
                    if (lq_v) begin
                        d_x <= lq_dsc[15:0];
                        d_y <= lq_dsc[31:16];
                        d_sptr <= lq_dsc[47:32];
                        d_pptr <= lq_dsc[63:48];
                        d_w <= {dec_n4w, 2'b00};
                        d_h <= {dec_n4h, 2'b00};
                        bpp_log <= dec_bpp;
                        bytes_per_row <= 10'((12'({4'd0, dec_n4w} << dec_bpp)
                                              + 12'd1) >> 1);
                        d_hflip <= custom && lq_dsc[76];
                        d_vflip <= custom && lq_dsc[77];
                        d_hdbl <= custom && lq_dsc[78];
                        d_vdbl <= custom && lq_dsc[79];
                        state <= M5_CLIP;
                    end
                M5_CLIP: begin
                    px_i <= a_img;
                    px_end <= 9'(d_w - 10'd1) - b_img;
                    dst <= d_hflip ? x_top : (d_x < 0 ? 10'd0 : d_x[9:0]);
                    lead <= d_hdbl && a_cut[0];
                    tail <= d_hdbl && b_cut[0];
                    pal_xram <= !d_pptr[0] && pal_fits(d_pptr, bpp_log);
                    row_addr <= {1'b0, d_sptr}
                        + 17'(17'({8'd0, row_sel})
                              * 17'({7'd0, bytes_per_row}));
                    if (tex_y >= {5'd0, h_dbl}
                        || x_f <= 17'sd0
                        || 17'(d_x) >= cw_s
                        || {1'b0, d_sptr} + data_size > 17'h10000)
                        next_sprite();
                    else
                        state <= M5_PIX;
                end
                M5_PIX: begin
                    if (!q_v)
                        pb <= pix_byte_addr[1:0];
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
    always_comb unused_mode5 = ^{attr[15:6], attr[2], lq_dsc[75:74],
                                     pix_byte_addr[16], bit_off_jj[2:0],
                                     end_byte[16], end_byte[1:0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
