/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 5, the paletted sprites of core/vga/mode5.c: an array of
 * descriptors — position, image, palette — each a fixed-size square at
 * 1, 2, 4 or 8 bits per pixel, walked in order so later sprites land on
 * earlier ones. The palette is read live per pixel, never snapshotted;
 * a color writes only where its alpha bit is set.
 */

module vid_mode5
    import vid_palette_pkg::*;
(
    input logic clk,

    input logic start,
    input logic abort_i,
    input logic [15:0] attr,
    input logic [15:0] cfg,
    input logic [15:0] length,
    input logic [8:0] t_row,
    input logic [9:0] cw,

    output logic vid_mode5_a_req,
    output logic [13:0] vid_mode5_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    /* On a miss the cache fills through this engine's own channel while
     * the pixel stalls. The two never request together: a palette lookup
     * only exists while the index word is already in hand. */
    output logic vid_mode5_pal_lookup,
    output logic vid_mode5_pal_xram,
    output logic vid_mode5_pal_one_bpp,
    output logic [15:0] vid_mode5_pal_base,
    output logic [7:0] vid_mode5_pal_idx,
    input logic pal_hit,
    input logic [15:0] pal_q,

    output logic vid_mode5_px_we,
    output logic [9:0] vid_mode5_px_addr,
    output logic [15:0] vid_mode5_px_data,

    output logic vid_mode5_done
);

    /* attr[5:3] the square's size, attr[1:0] the depth; the prog
     * validated the pairing.
     *
     * Taken once at the start rather than re-derived: the derivation is
     * the plane's slot mux, two shifts and a seventeen-bit multiply, and
     * it stood in front of every decision the walk makes. Nothing
     * downstream reads these before M5_JUDGE. */
    logic [3:0] size_log;
    logic [9:0] size_w, bytes_per_row_w;
    always_comb begin
        size_log = 4'd3 + {1'b0, attr[5:3]};
        size_w = 10'(10'd1 << size_log);
        bytes_per_row_w = 10'(13'({3'd0, size_w} << attr[1:0]) >> 3);
    end
    logic [1:0] bpp_log;
    logic [9:0] size;
    logic [9:0] bytes_per_row;
    logic [16:0] data_size;

    typedef enum logic [2:0] {
        M5_IDLE, M5_DESC, M5_JUDGE, M5_PIX, M5_NEXT
    } state_t;
    state_t state;

    logic [15:0] idx;

    /* Entered at the top, so the descriptor ends flush against bit 63
     * wherever it started and the junk halfword ahead of it falls out
     * the bottom. */
    logic [63:0] gather;
    logic [15:0] hi_hold;
    logic hi_pend;
    logic [16:0] daddr;
    always_comb daddr = {1'b0, cfg} + {1'd0, idx[12:0], 3'b000};
    logic [2:0] fw_i, fw_n, sh_c, sh_n;
    logic gnt_d;
    logic [15:0] sh_in;
    always_comb sh_in = gnt_d ? a_rdata[15:0] : hi_hold;
    logic signed [15:0] d_x, d_y;
    logic [15:0] d_sptr, d_pptr;
    always_comb begin
        d_x = gather[15:0];
        d_y = gather[31:16];
        d_sptr = gather[47:32];
        d_pptr = gather[63:48];
    end

    logic [15:0] tex_y;
    always_comb tex_y = {7'd0, t_row} - 16'(d_y);

    /* The span end under C's promoted compare: size_x assignments stay
     * int16, the against-width clamp does not overflow. */
    logic signed [17:0] clip_end;
    logic signed [15:0] clip_size_x;
    always_comb begin
        clip_size_x = d_x < 0 ? 16'($signed({6'd0, size} + 16'(d_x)))
                              : $signed({6'd0, size});
        clip_end = (d_x < 0 ? 18'sd0 : 18'(d_x)) + 18'(clip_size_x);
        if (clip_end > 18'($signed({8'd0, cw})))
            clip_size_x = 16'($signed({6'd0, cw})
                              - (d_x < 0 ? 16'sd0 : d_x));
    end

    logic signed [15:0] tex_x, size_x;
    logic pal_xram;
    logic [16:0] row_addr;

    /* One cached XRAM word feeds the index bytes: while one word emits,
     * the spare clocks ask for the next, and a boundary costs one clock
     * promoting rather than a fetch's round trip. Emitting straight from
     * the prefetch would buy that clock back with a mux ahead of the
     * palette lookup, which is why mode 4 does it and this does not. */
    logic [31:0] dcache;
    logic [13:0] dcache_word;
    logic dcache_v;
    logic [31:0] pre_data;
    logic [13:0] pre_word;
    logic pre_v;     /* the next word is here */
    logic pre_pend;  /* ...or it has been asked for */
    logic signed [15:0] px_i;   /* pixel within the sprite row */
    logic [9:0] dst;

    logic [16:0] pix_byte_addr;
    always_comb pix_byte_addr = row_addr
        + {4'd0, 13'(16'(px_i) << bpp_log) >> 3};
    logic [7:0] cur_byte;
    always_comb cur_byte = dcache[{pix_byte_addr[1:0], 3'b000}+:8];
    logic [2:0] bit_off;
    always_comb bit_off = 3'(16'(px_i) << bpp_log);
    logic [7:0] pix_idx;
    always_comb begin
        case (bpp_log)
            2'd0: pix_idx = {7'd0, cur_byte[3'd7 - bit_off]};
            2'd1: pix_idx = {6'd0, cur_byte[{2'd3 - bit_off[2:1], 1'b0}+:2]};
            2'd2: pix_idx = {4'd0, cur_byte[{!bit_off[2], 2'b00}+:4]};
            default: pix_idx = cur_byte;
        endcase
    end
    /* The cache resolves XRAM and builtin palettes alike into a
     * finished color; this engine only names the question. */
    always_comb begin
        vid_mode5_pal_lookup = state == M5_PIX && dhit && pal_xram;
        vid_mode5_pal_xram = pal_xram;
        vid_mode5_pal_one_bpp = bpp_log == 2'd0;
        vid_mode5_pal_base = d_pptr;
        vid_mode5_pal_idx = pix_idx;
    end
    logic [15:0] pal_color;
    always_comb pal_color = pal_q;

    /* No address compare: a prefetch is only ever issued from a hit at
     * dcache_word + 1 and the walk is sequential, so two fourteen-bit
     * comparators off the pixel address adder would prove what the walk
     * already guarantees. */
    logic dhit;
    always_comb dhit = dcache_v && dcache_word == pix_byte_addr[15:2];
    logic [13:0] pre_next;
    always_comb pre_next = dcache_word + 14'd1;
    logic pre_want;
    always_comb pre_want = state == M5_PIX && dhit
        && !pre_v && !pre_pend;

    always_comb begin
        vid_mode5_a_req = 1'b0;
        vid_mode5_a_addr = daddr[15:2] + {11'd0, fw_i};
        case (state)
            /* One word in flight: the half held back has to shift before the
             * next word's low half arrives, and dropping the request for the
             * grant's own clock is what spaces them. */
            M5_DESC: vid_mode5_a_req = fw_i < fw_n && !gnt_d;
            M5_PIX: begin
                /* A prefetch of this word may still be in flight; a
                 * duplicate miss fetch would land on a clock the promote
                 * path already covers, leaving fw_i raised and the
                 * request line silent. */
                if (!dhit && !pre_v && !pre_pend) begin
                    vid_mode5_a_req = fw_i == 3'd0;
                    vid_mode5_a_addr = pix_byte_addr[15:2];
                end else if (pre_want) begin
                    vid_mode5_a_req = 1'b1;
                    vid_mode5_a_addr = pre_next;
                end
            end
            default: ;
        endcase
    end

    /* The write lands only where the color carries alpha, and only when
     * the cache has answered — a miss stalls the pixel, not the walk's
     * correctness. Builtin palettes always hit. */
    always_comb begin
        vid_mode5_px_we = 1'b0;
        vid_mode5_px_addr = dst;
        vid_mode5_px_data = pal_color;
        if (state == M5_PIX && dhit && pal_hit)
            vid_mode5_px_we = pal_color[5];
    end

    task automatic next_sprite();
        /* Whatever was read ahead belonged to the sprite just finished. */
        pre_v <= 1'b0;
        pre_pend <= 1'b0;
        if (idx + 16'd1 == length) begin
            vid_mode5_done <= 1'b1;
            state <= M5_IDLE;
        end else begin
            idx <= idx + 16'd1;
            state <= M5_NEXT;
        end
    endtask

    task automatic step_pixel();
        if (px_i == tex_x + size_x - 16'sd1)
            next_sprite();
        else begin
            px_i <= px_i + 16'sd1;
            dst <= dst + 10'd1;
        end
    endtask

    initial begin
        state = M5_IDLE;
        idx = '0;
        gather = '0;
        hi_hold = '0;
        hi_pend = 1'b0;
        fw_i = '0;
        fw_n = '0;
        sh_c = '0;
        sh_n = '0;
        gnt_d = 1'b0;
        tex_x = '0;
        size_x = '0;
        pal_xram = 1'b0;
        row_addr = '0;
        dcache = '0;
        dcache_word = '0;
        dcache_v = 1'b0;
        pre_data = '0;
        pre_word = '0;
        pre_v = 1'b0;
        pre_pend = 1'b0;
        px_i = '0;
        dst = '0;
        bpp_log = '0;
        size = '0;
        bytes_per_row = '0;
        data_size = '0;
        vid_mode5_done = 1'b0;
    end
    always_ff @(posedge clk) begin
        gnt_d <= a_gnt;
        vid_mode5_done <= 1'b0;
        if (abort_i) begin
            /* A lost race: the scaffold counted it; drop the line. */
            state <= M5_IDLE;
        end else if (start) begin
            idx <= '0;
            dcache_v <= 1'b0;
            pre_v <= 1'b0;
            pre_pend <= 1'b0;
            bpp_log <= attr[1:0];
            size <= size_w;
            bytes_per_row <= bytes_per_row_w;
            data_size <= 17'(17'({7'd0, size_w})
                             * 17'({7'd0, bytes_per_row_w}));
            if (length == 16'd0) begin
                vid_mode5_done <= 1'b1;
                state <= M5_IDLE;
            end else
                state <= M5_NEXT;
        end else begin
            case (state)
                M5_IDLE: ;
                M5_NEXT: begin
                    /* Aim the gather at descriptor idx. cfg[1] rather than
                     * daddr[1]: the stride is a multiple of four, so the
                     * array's alignment is every descriptor's, and the
                     * address adder stays out of it. */
                    fw_i <= '0;
                    sh_c <= '0;
                    hi_pend <= 1'b0;
                    fw_n <= cfg[1] ? 3'd3 : 3'd2;
                    sh_n <= cfg[1] ? 3'd5 : 3'd4;
                    state <= M5_DESC;
                end
                M5_DESC: begin
                    if (a_gnt)
                        fw_i <= fw_i + 3'd1;
                    hi_pend <= gnt_d;
                    if (gnt_d)
                        hi_hold <= a_rdata[31:16];
                    if (gnt_d || hi_pend) begin
                        gather <= {sh_in, gather[63:16]};
                        sh_c <= sh_c + 3'd1;
                        if (sh_c + 3'd1 == sh_n)
                            state <= M5_JUDGE;
                    end
                end
                M5_JUDGE: begin
                    tex_x <= d_x < 0 ? -d_x : 16'sd0;
                    size_x <= clip_size_x;
                    pal_xram <= !d_pptr[0]
                        && {1'b0, d_pptr}
                            <= 17'h10000
                                - (17'd2 << {12'd0, 5'd1 << bpp_log});
                    row_addr <= {1'b0, d_sptr}
                        + 17'(17'({8'd0, tex_y[8:0]})
                              * 17'({7'd0, bytes_per_row}));
                    px_i <= d_x < 0 ? -d_x : 16'sd0;
                    dst <= d_x < 0 ? 10'd0 : d_x[9:0];
                    fw_i <= '0;
                    if (tex_y >= {6'd0, size}
                        || clip_size_x < 16'sd1
                        || {1'b0, d_sptr} > 17'h10000 - data_size)
                        next_sprite();
                    else
                        state <= M5_PIX;
                end
                M5_PIX: begin
                    /* The prefetch's own answer, told apart from the
                     * miss fetch's by which one is pending — only ever
                     * one is outstanding, because the request logic
                     * asks for one or the other. */
                    if (pre_pend && gnt_d) begin
                        pre_data <= a_rdata;
                        pre_v <= 1'b1;
                        pre_pend <= 1'b0;
                    end else if (pre_want && a_gnt) begin
                        pre_word <= pre_next;
                        pre_pend <= 1'b1;
                    end
                    if (!dhit) begin
                        if (pre_v) begin
                            dcache <= pre_data;
                            dcache_word <= pre_word;
                            dcache_v <= 1'b1;
                            pre_v <= 1'b0;
                        end else begin
                            if (a_gnt && !pre_want) begin
                                fw_i <= 3'd1;
                                dcache_word <= pix_byte_addr[15:2];
                                dcache_v <= 1'b0;
                            end
                            if (gnt_d && !pre_pend) begin
                                dcache <= a_rdata;
                                dcache_v <= 1'b1;
                                fw_i <= '0;
                            end
                        end
                    end else if (pal_hit)
                        step_pixel();
                    /* else: the cache is filling on this channel. */
                end
                default: state <= M5_IDLE;
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_mode5;
    always_comb unused_vid_mode5 = ^{attr[15:6], attr[2], gather,
                                     daddr[16], daddr[1:0], tex_y[15:9],
                                     pix_byte_addr[16],
                                     idx[15:13]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
