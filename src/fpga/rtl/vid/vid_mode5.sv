/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 5, the paletted sprites of vga/modes/mode5.c: an array of
 * descriptors — position, image, palette — each a fixed-size square at
 * 1, 2, 4 or 8 bits per pixel, walked in order so later sprites land on
 * earlier ones. The palette is read live per pixel like the C reads
 * through its pointer, never snapshotted; a color writes only where its
 * alpha bit is set, over whatever the target plane already holds. The
 * sprite scaffold starts one line's walk with the slot in hand and
 * routes the pixel port at the foreground plane; only the beam's
 * deadline matters.
 */

module vid_mode5
    import vid_palette_pkg::*;
(
    input logic clk,
    input logic rst_n,

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

    /* The palette, asked of the sprite stage's cache: a finished color
     * and a hit, combinational. On a miss the cache fills through this
     * engine's own channel while the pixel stalls — the two never
     * request together, because a palette lookup only exists while the
     * index word is already in hand. */
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

    /* attr[5:3] the square's size — eight through five hundred twelve —
     * attr[1:0] the depth; the prog validated the pairing.
     *
     * Taken once at the start rather than re-derived every clock. attr is
     * the slot's and does not move for the length of a plane's walk, but
     * the derivation was standing in front of every decision the walk
     * makes: the plane's slot mux, two shifts and a seventeen-bit
     * multiply, nine nanoseconds of the twenty available, reached before
     * the skip ladder's compare could start. Nothing downstream reads
     * these before M5_JUDGE, which is three clocks after the start. */
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

    /* The descriptor gather: eight bytes from a halfword-aligned array,
     * two or three words. */
    logic [95:0] gather;
    logic [1:0] lane;
    logic [16:0] daddr;
    always_comb daddr = {1'b0, cfg} + {1'd0, idx[12:0], 3'b000};
    logic [1:0] daddr_lane;
    always_comb daddr_lane = daddr[1:0];
    logic [2:0] fw_i, fw_c, fw_n;
    logic gnt_d;
    logic [63:0] dview;
    always_comb dview = 64'(gather >> {lane, 3'b000});
    logic signed [15:0] d_x, d_y;
    logic [15:0] d_sptr, d_pptr;
    always_comb begin
        d_x = dview[15:0];
        d_y = dview[31:16];
        d_sptr = dview[47:32];
        d_pptr = dview[63:48];
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

    /* One cached XRAM word feeds the index bytes. */
    logic [31:0] dcache;
    logic [13:0] dcache_word;
    logic dcache_v;
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

    logic dhit;
    always_comb dhit = dcache_v && dcache_word == pix_byte_addr[15:2];

    always_comb begin
        vid_mode5_a_req = 1'b0;
        vid_mode5_a_addr = daddr[15:2] + {11'd0, fw_i};
        case (state)
            M5_DESC: vid_mode5_a_req = fw_i < fw_n;
            M5_PIX: begin
                if (!dhit) begin
                    vid_mode5_a_req = fw_i == 3'd0;
                    vid_mode5_a_addr = pix_byte_addr[15:2];
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

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= M5_IDLE;
            idx <= '0;
            gather <= '0;
            lane <= '0;
            fw_i <= '0;
            fw_c <= '0;
            fw_n <= '0;
            gnt_d <= 1'b0;
            tex_x <= '0;
            size_x <= '0;
            pal_xram <= 1'b0;
            row_addr <= '0;
            dcache <= '0;
            dcache_word <= '0;
            dcache_v <= 1'b0;
            px_i <= '0;
            dst <= '0;
            bpp_log <= '0;
            size <= '0;
            bytes_per_row <= '0;
            data_size <= '0;
            vid_mode5_done <= 1'b0;
        end else begin
            gnt_d <= a_gnt;
            vid_mode5_done <= 1'b0;
            if (abort_i) begin
                /* A lost race: the scaffold counted it; drop the line. */
                state <= M5_IDLE;
            end else if (start) begin
                idx <= '0;
                dcache_v <= 1'b0;
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
                        /* Aim the gather at descriptor idx. */
                        lane <= daddr_lane;
                        fw_i <= '0;
                        fw_c <= '0;
                        fw_n <= 3'((5'({3'd0, daddr_lane}) + 5'd8 + 5'd3)
                                   >> 2);
                        gather <= '0;
                        state <= M5_DESC;
                    end
                    M5_DESC: begin
                        if (a_gnt)
                            fw_i <= fw_i + 3'd1;
                        if (gnt_d) begin
                            case (fw_c)
                                3'd0: gather[31:0] <= a_rdata;
                                3'd1: gather[63:32] <= a_rdata;
                                3'd2: gather[95:64] <= a_rdata;
                                default: ;
                            endcase
                            fw_c <= fw_c + 3'd1;
                            if (fw_c + 3'd1 == fw_n)
                                state <= M5_JUDGE;
                        end
                    end
                    M5_JUDGE: begin
                        /* The oracle's skip ladder, one clock. */
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
                        if (!dhit) begin
                            /* The index word misses; refetch. */
                            if (a_gnt) begin
                                fw_i <= 3'd1;
                                dcache_word <= pix_byte_addr[15:2];
                                dcache_v <= 1'b0;
                            end
                            if (gnt_d) begin
                                dcache <= a_rdata;
                                dcache_v <= 1'b1;
                                fw_i <= '0;
                            end
                        end else if (pal_hit)
                            step_pixel();
                        /* else: the cache is filling on this channel. */
                    end
                    default: state <= M5_IDLE;
                endcase
            end
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_mode5;
    always_comb unused_vid_mode5 = ^{attr[15:6], attr[2], gather,
                                     daddr[16], tex_y[15:9],
                                     pix_byte_addr[16],
                                     idx[15:13], dview};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
