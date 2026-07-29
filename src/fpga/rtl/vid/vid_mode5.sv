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

    output logic vid_mode5_px_we,
    output logic [9:0] vid_mode5_px_addr,
    output logic [15:0] vid_mode5_px_data,

    output logic vid_mode5_done
);

    /* attr[5:3] the square's size — eight through five hundred twelve —
     * attr[1:0] the depth; the prog validated the pairing. */
    logic [1:0] bpp_log;
    logic [3:0] size_log;
    always_comb begin
        bpp_log = attr[1:0];
        size_log = 4'd3 + {1'b0, attr[5:3]};
    end
    logic [9:0] size;
    always_comb size = 10'(10'd1 << size_log);
    logic [9:0] bytes_per_row;
    always_comb bytes_per_row = 10'(13'({3'd0, size} << bpp_log) >> 3);
    logic [16:0] data_size;
    always_comb data_size =
        17'(17'({7'd0, size}) * 17'({7'd0, bytes_per_row}));

    typedef enum logic [2:0] {
        M5_IDLE, M5_DESC, M5_JUDGE, M5_PIX, M5_PAL, M5_NEXT
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
    /* One word carries four texels at eight bits and eight at four, so
     * the walk has clocks to spare between misses. The spare one asks
     * for the next word — see vid_mode4, same shape, same reason. */
    logic [31:0] dcache;
    logic [13:0] dcache_word;
    logic dcache_v;
    logic dhit_c, pre_hit;
    logic [31:0] hit_data;
    logic [31:0] pre_data;
    logic [13:0] pre_word;
    logic pre_v;
    logic pre_pend;
    logic [13:0] pre_next;
    logic pre_want;
    logic signed [15:0] px_i;   /* pixel within the sprite row */
    logic [9:0] dst;

    logic [16:0] pix_byte_addr;
    always_comb pix_byte_addr = row_addr
        + {4'd0, 13'(16'(px_i) << bpp_log) >> 3};
    logic [7:0] cur_byte;
    always_comb cur_byte = hit_data[{pix_byte_addr[1:0], 3'b000}+:8];
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
    logic [16:0] pal_addr;
    always_comb pal_addr = {1'b0, d_pptr} + {8'd0, pix_idx, 1'b0};
    logic [15:0] pal_color;
    always_comb pal_color = pal_xram
        ? (pal_addr[1] ? a_rdata[31:16] : a_rdata[15:0])
        : (bpp_log == 2'd0 ? VID_COLOR_2[pix_idx[0]]
                           : VID_COLOR_256[pix_idx]);

    logic dhit;
    always_comb begin
        dhit_c = dcache_v && dcache_word == pix_byte_addr[15:2];
        pre_hit = state == M5_PIX && pre_v
            && pre_word == pix_byte_addr[15:2];
        dhit = dhit_c || pre_hit;
        hit_data = dhit_c ? dcache : pre_data;
        pre_next = (pre_hit ? pre_word : dcache_word) + 14'd1;
        pre_want = state == M5_PIX && dhit && dcache_v && !pal_xram
            && !pre_v && !pre_pend;
    end

    always_comb begin
        vid_mode5_a_req = 1'b0;
        vid_mode5_a_addr = daddr[15:2] + {11'd0, fw_i};
        case (state)
            M5_DESC: vid_mode5_a_req = fw_i < fw_n;
            M5_PIX: begin
                if (!dhit) begin
                    vid_mode5_a_req = fw_i == 3'd0;
                    vid_mode5_a_addr = pix_byte_addr[15:2];
                end else if (pal_xram) begin
                    vid_mode5_a_req = fw_i == 3'd0;
                    vid_mode5_a_addr = pal_addr[15:2];
                end else if (pre_want) begin
                    vid_mode5_a_req = 1'b1;
                    vid_mode5_a_addr = pre_next;
                end
            end
            default: ;
        endcase
    end

    /* The write lands only where the color carries alpha. */
    always_comb begin
        vid_mode5_px_we = 1'b0;
        vid_mode5_px_addr = dst;
        vid_mode5_px_data = pal_color;
        if (state == M5_PIX && dhit && !pal_xram)
            vid_mode5_px_we = pal_color[5];
        else if (state == M5_PAL && gnt_d)
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
            pre_data <= '0;
            pre_word <= '0;
            pre_v <= 1'b0;
            pre_pend <= 1'b0;
            px_i <= '0;
            dst <= '0;
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
                pre_v <= 1'b0;
                pre_pend <= 1'b0;
                if (length == 16'd0) begin
                    vid_mode5_done <= 1'b1;
                    state <= M5_IDLE;
                end else
                    state <= M5_NEXT;
            end else begin
                case (state)
                    M5_IDLE: ;
                    M5_NEXT: begin
                        /* Aim the gather at descriptor idx. Whatever was
                         * read ahead belonged to the sprite before. */
                        pre_v <= 1'b0;
                        pre_pend <= 1'b0;
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
                        if (pre_pend && gnt_d) begin
                            pre_data <= a_rdata;
                            pre_v <= 1'b1;
                            pre_pend <= 1'b0;
                        end else if (pre_want && a_gnt) begin
                            pre_word <= pre_next;
                            pre_pend <= 1'b1;
                        end
                        if (pre_hit) begin
                            dcache <= pre_data;
                            dcache_word <= pre_word;
                            dcache_v <= 1'b1;
                            pre_v <= 1'b0;
                        end
                        if (!dhit) begin
                            /* The index word misses; refetch. */
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
                        end else if (pal_xram) begin
                            if (a_gnt) begin
                                fw_i <= 3'd1;
                                state <= M5_PAL;
                            end
                        end else
                            step_pixel();
                    end
                    M5_PAL: begin
                        if (gnt_d) begin
                            fw_i <= '0;
                            state <= M5_PIX;
                            step_pixel();
                        end
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
                                     pal_addr[16], pal_addr[0],
                                     pix_byte_addr[16],
                                     idx[15:13], dview};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
