/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 4, the sixteen-bit sprites of vga/modes/mode4.c — the RISCBoy
 * system pico-playground carries: an array of descriptors, each a
 * power-of-two square of raw RGB555 texels, walked in order so later
 * sprites land on earlier ones. A texel writes where its alpha bit is
 * set; a sprite with opacity metadata narrows each row to its opaque
 * span first and skips the alpha test entirely when the row is marked
 * continuous. The affine path joins here. The sprite scaffold starts
 * one line's walk with the slot in hand and routes the pixel port at
 * the foreground plane; only the beam's deadline matters.
 */

module vid_mode4 (
    input logic clk,
    input logic rst_n,

    input logic start,
    input logic abort_i,
    input logic [15:0] cfg,
    input logic [15:0] length,
    input logic [8:0] t_row,
    input logic [9:0] cw,

    output logic vid_mode4_a_req,
    output logic [13:0] vid_mode4_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    output logic vid_mode4_px_we,
    output logic [9:0] vid_mode4_px_addr,
    output logic [15:0] vid_mode4_px_data,

    output logic vid_mode4_done
);

    typedef enum logic [2:0] {
        M4_IDLE, M4_NEXT, M4_DESC, M4_JUDGE, M4_META, M4_PIX
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
    logic [15:0] d_sptr;
    logic [7:0] d_log;
    logic d_meta;
    always_comb begin
        d_x = dview[15:0];
        d_y = dview[31:16];
        d_sptr = dview[47:32];
        d_log = dview[55:48];
        d_meta = dview[63:56] != 8'h00;
    end

    /* The square's geometry; log sizes past seven cannot fit XRAM and
     * skip on the size test exactly as the oracle's arithmetic does. */
    logic [7:0] size;
    always_comb size = d_log[7:3] == 5'd0 ? 8'(8'd1 << d_log[2:0]) : 8'd0;
    logic log_big;
    always_comb log_big = d_log[7:3] != 5'd0;
    logic [16:0] img_bytes;
    always_comb img_bytes = 17'(17'd2 << {13'd0, d_log[2:0], 1'b0});
    logic [17:0] byte_size;
    always_comb byte_size = {1'b0, img_bytes}
        + (d_meta ? 18'({size, 2'b00}) : 18'd0);

    /* The intersect, in the oracle's int arithmetic. */
    logic signed [16:0] tex_offs_y;
    always_comb tex_offs_y = 17'($signed({8'd0, t_row}) - 17'(d_y));
    logic signed [16:0] x_start;
    always_comb x_start = d_x < 0 ? 17'sd0 : 17'(d_x);
    logic signed [16:0] tex_offs_x0;
    always_comb tex_offs_x0 = x_start - 17'(d_x);
    logic signed [17:0] span_rhs;
    always_comb span_rhs = 18'(d_x) + 18'($signed({10'd0, size}));
    logic signed [17:0] size_x0;
    always_comb size_x0 =
        (span_rhs < 18'($signed({8'd0, cw})) ? span_rhs
                                             : 18'($signed({8'd0, cw})))
        - 18'(x_start);

    /* Clipped span registers, adjusted again by metadata. */
    logic signed [16:0] tex_x, span_end;
    logic meta_cont;
    logic [16:0] row_texel;  /* texel index of the row's first column */

    logic [16:0] meta_addr;
    always_comb meta_addr = {1'b0, d_sptr} + img_bytes[16:0]
        + {8'd0, tex_offs_y[6:0], 2'b00};

    /* One cached XRAM word carries two texels. */
    logic [31:0] dcache;
    logic [13:0] dcache_word;
    logic dcache_v;
    logic signed [16:0] px_i;
    logic [9:0] dst;

    logic [17:0] tex_byte_addr;
    always_comb tex_byte_addr = {1'b0, d_sptr}
        + {(17'(row_texel) + 17'(px_i[15:0])), 1'b0};
    logic [15:0] texel;
    always_comb texel = tex_byte_addr[1] ? dcache[31:16] : dcache[15:0];
    logic dhit;
    always_comb dhit = dcache_v && dcache_word == tex_byte_addr[15:2];

    always_comb begin
        vid_mode4_a_req = 1'b0;
        vid_mode4_a_addr = daddr[15:2] + {11'd0, fw_i};
        case (state)
            M4_DESC: vid_mode4_a_req = fw_i < fw_n;
            M4_META: begin
                vid_mode4_a_req = fw_i == 3'd0;
                vid_mode4_a_addr = meta_addr[15:2];
            end
            M4_PIX: begin
                if (!dhit) begin
                    vid_mode4_a_req = fw_i == 3'd0;
                    vid_mode4_a_addr = tex_byte_addr[15:2];
                end
            end
            default: ;
        endcase
    end

    always_comb begin
        vid_mode4_px_we = 1'b0;
        vid_mode4_px_addr = dst;
        vid_mode4_px_data = texel;
        if (state == M4_PIX && dhit)
            vid_mode4_px_we = meta_cont || texel[5];
    end

    task automatic next_sprite();
        if (idx + 16'd1 == length) begin
            vid_mode4_done <= 1'b1;
            state <= M4_IDLE;
        end else begin
            idx <= idx + 16'd1;
            state <= M4_NEXT;
        end
    endtask

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= M4_IDLE;
            idx <= '0;
            gather <= '0;
            lane <= '0;
            fw_i <= '0;
            fw_c <= '0;
            fw_n <= '0;
            gnt_d <= 1'b0;
            tex_x <= '0;
            span_end <= '0;
            meta_cont <= 1'b0;
            row_texel <= '0;
            dcache <= '0;
            dcache_word <= '0;
            dcache_v <= 1'b0;
            px_i <= '0;
            dst <= '0;
            vid_mode4_done <= 1'b0;
        end else begin
            gnt_d <= a_gnt;
            vid_mode4_done <= 1'b0;
            if (abort_i) begin
                if (state != M4_IDLE)
                    $fatal(1, "vid_mode4 underrun");
            end else if (start) begin
                idx <= '0;
                dcache_v <= 1'b0;
                if (length == 16'd0) begin
                    vid_mode4_done <= 1'b1;
                    state <= M4_IDLE;
                end else
                    state <= M4_NEXT;
            end else begin
                case (state)
                    M4_IDLE: ;
                    M4_NEXT: begin
                        lane <= daddr_lane;
                        fw_i <= '0;
                        fw_c <= '0;
                        fw_n <= 3'((5'({3'd0, daddr_lane}) + 5'd8 + 5'd3)
                                   >> 2);
                        gather <= '0;
                        state <= M4_DESC;
                    end
                    M4_DESC: begin
                        if (a_gnt)
                            fw_i <= fw_i + 3'd1;
                        if (gnt_d) begin
                            gather <= gather
                                | (96'(a_rdata) << {fw_c, 5'd0});
                            fw_c <= fw_c + 3'd1;
                            if (fw_c + 3'd1 == fw_n)
                                state <= M4_JUDGE;
                        end
                    end
                    M4_JUDGE: begin
                        tex_x <= tex_offs_x0;
                        span_end <= tex_offs_x0 + 17'(size_x0[16:0]);
                        meta_cont <= 1'b0;
                        row_texel <= 17'(17'(tex_offs_y[6:0])
                                         << d_log[2:0]);
                        px_i <= tex_offs_x0;
                        dst <= 10'(x_start);
                        fw_i <= '0;
                        if (log_big
                            || byte_size > 18'h10000
                            || {2'b0, d_sptr} > 18'h10000 - byte_size
                            || tex_offs_y < 0
                            || tex_offs_y >= 17'($signed({9'd0, size}))
                            || size_x0 < 18'sd1)
                            next_sprite();
                        else if (d_meta)
                            state <= M4_META;
                        else
                            state <= M4_PIX;
                    end
                    M4_META: begin
                        if (a_gnt)
                            fw_i <= 3'd1;
                        if (gnt_d) begin
                            /* Narrow to the row's opaque span; the walk
                             * below skips when it comes up empty. */
                            if (17'($signed({2'd0, a_rdata[30:16]}))
                                > tex_x) begin
                                tex_x <= 17'({2'd0, a_rdata[30:16]});
                                px_i <= 17'({2'd0, a_rdata[30:16]});
                                dst <= 10'(17'(d_x)
                                           + 17'({2'd0, a_rdata[30:16]}));
                            end
                            if (17'($signed({1'd0, a_rdata[15:0]}))
                                < span_end)
                                span_end <= 17'({1'd0, a_rdata[15:0]});
                            meta_cont <= a_rdata[31];
                            fw_i <= '0;
                            state <= M4_PIX;
                        end
                    end
                    M4_PIX: begin
                        if (px_i >= span_end)
                            next_sprite();
                        else if (!dhit) begin
                            if (a_gnt) begin
                                fw_i <= 3'd1;
                                dcache_word <= tex_byte_addr[15:2];
                                dcache_v <= 1'b0;
                            end
                            if (gnt_d) begin
                                dcache <= a_rdata;
                                dcache_v <= 1'b1;
                                fw_i <= '0;
                            end
                        end else begin
                            px_i <= px_i + 17'sd1;
                            dst <= dst + 10'd1;
                        end
                    end
                    default: state <= M4_IDLE;
                endcase
            end
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_mode4;
    always_comb unused_vid_mode4 = ^{gather, dview, daddr[16],
                                     meta_addr[16], meta_addr[1:0],
                                     tex_byte_addr[17:16],
                                     tex_byte_addr[0], size_x0[17],
                                     img_bytes, tex_x};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
