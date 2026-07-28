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
    input logic [15:0] attr,
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

    /* attr 1 is the affine walk over twenty-byte descriptors. */
    logic affine;
    always_comb affine = attr[0];

    typedef enum logic [3:0] {
        M4_IDLE, M4_NEXT, M4_DESC, M4_JUDGE, M4_META, M4_PIX,
        M4_ASETUP, M4_APOP
    } state_t;
    state_t state;

    logic [15:0] idx;

    /* The descriptor gather: eight or twenty bytes from a
     * halfword-aligned array, up to six words. */
    logic [191:0] gather;
    logic [1:0] lane;
    logic [16:0] daddr;
    always_comb daddr = {1'b0, cfg}
        + (affine
           ? 17'(17'(idx[12:0]) * 17'd20)
           : {1'd0, idx[12:0], 3'b000});
    logic [1:0] daddr_lane;
    always_comb daddr_lane = daddr[1:0];
    logic [2:0] fw_i, fw_c, fw_n;
    logic gnt_d;
    logic [159:0] dview;
    always_comb dview = 160'(gather >> {lane, 3'b000});
    logic signed [15:0] d_x, d_y;
    logic [15:0] d_sptr;
    logic [7:0] d_log;
    logic d_meta;
    logic signed [15:0] d_t[6];
    always_comb begin
        for (int j = 0; j < 6; j++)
            d_t[j] = dview[16 * j+:16];
        d_x = affine ? dview[111:96] : dview[15:0];
        d_y = affine ? dview[127:112] : dview[31:16];
        d_sptr = affine ? dview[143:128] : dview[47:32];
        d_log = affine ? dview[151:144] : dview[55:48];
        d_meta = (affine ? dview[159:152] : dview[63:56]) != 8'h00;
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

    /* The affine accumulators, the SIO interpolator's arithmetic: the
     * first sample sits one past the span and the walk runs backward,
     * subtracting a00 and a10 in wrapping thirty-two bits — the exact
     * uint32 stream the oracle's software interpolator produces. */
    logic signed [31:0] af_a00, af_a10;
    always_comb af_a00 = {{8{d_t[0][15]}}, d_t[0], 8'd0};
    always_comb af_a10 = {{8{d_t[3][15]}}, d_t[3], 8'd0};
    logic [31:0] af_u, af_v;
    logic [16:0] af_left;   /* pops remaining */
    logic af_over;
    always_comb af_over =
        ((af_u | af_v) & (32'hFFFF0000 << d_log[2:0])) != 32'd0;
    logic [17:0] af_byte_addr;
    always_comb begin
        logic [6:0] ui, vi;
        ui = 7'(af_u[22:16] & 7'((8'd1 << d_log[2:0]) - 8'd1));
        vi = 7'(af_v[22:16] & 7'((8'd1 << d_log[2:0]) - 8'd1));
        af_byte_addr = {2'b0, d_sptr}
            + {10'd0, ui, 1'b0}
            + (18'({11'd0, vi}) << (d_log[2:0] + 4'd1));
    end

    /* The span's first sample, one past its end — four fixed-point
     * multiplies, every term wrapping mod 2^32 like the oracle's. */
    logic signed [31:0] af_u0, af_v0;
    always_comb begin
        logic signed [17:0] kx;
        kx = 18'(tex_offs_x0) + size_x0;
        af_u0 = 32'(32'({{8{d_t[0][15]}}, d_t[0], 8'd0}) * 32'(kx)
                    + 32'({{8{d_t[1][15]}}, d_t[1], 8'd0})
                      * 32'(tex_offs_y)
                    + 32'({{8{d_t[2][15]}}, d_t[2], 8'd0}));
        af_v0 = 32'(32'({{8{d_t[3][15]}}, d_t[3], 8'd0}) * 32'(kx)
                    + 32'({{8{d_t[4][15]}}, d_t[4], 8'd0})
                      * 32'(tex_offs_y)
                    + 32'({{8{d_t[5][15]}}, d_t[5], 8'd0}));
    end

    logic [17:0] cur_byte_addr;
    always_comb cur_byte_addr =
        state == M4_APOP ? af_byte_addr : tex_byte_addr;
    logic [15:0] texel;
    always_comb texel = cur_byte_addr[1] ? dcache[31:16] : dcache[15:0];
    logic dhit;
    always_comb dhit = dcache_v && dcache_word == cur_byte_addr[15:2];

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
            M4_APOP: begin
                if (!af_over && !dhit) begin
                    vid_mode4_a_req = fw_i == 3'd0;
                    vid_mode4_a_addr = af_byte_addr[15:2];
                end
            end
            default: ;
        endcase
    end

    always_comb begin
        vid_mode4_px_we = 1'b0;
        vid_mode4_px_addr = dst;
        vid_mode4_px_data = texel;
        if (state == M4_PIX && dhit && px_i < span_end)
            vid_mode4_px_we = meta_cont || texel[5];
        else if (state == M4_APOP && !af_over && dhit
                 && af_left != 17'd0)
            vid_mode4_px_we = texel[5];
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

    task automatic step_af();
        af_u <= af_u - 32'(af_a00);
        af_v <= af_v - 32'(af_a10);
        dst <= dst - 10'd1;
        af_left <= af_left - 17'd1;
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
            af_u <= '0;
            af_v <= '0;
            af_left <= '0;
            vid_mode4_done <= 1'b0;
        end else begin
            gnt_d <= a_gnt;
            vid_mode4_done <= 1'b0;
            if (abort_i) begin
                /* A lost race: the scaffold counted it; drop the line. */
                state <= M4_IDLE;
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
                        fw_n <= 3'((6'({4'd0, daddr_lane})
                                    + (affine ? 6'd20 : 6'd8) + 6'd3)
                                   >> 2);
                        gather <= '0;
                        state <= M4_DESC;
                    end
                    M4_DESC: begin
                        if (a_gnt)
                            fw_i <= fw_i + 3'd1;
                        if (gnt_d) begin
                            gather <= gather
                                | (192'(a_rdata) << {fw_c, 5'd0});
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
                        else if (affine)
                            state <= M4_ASETUP;
                        else if (d_meta)
                            state <= M4_META;
                        else
                            state <= M4_PIX;
                    end
                    M4_ASETUP: begin
                        af_u <= 32'(af_u0);
                        af_v <= 32'(af_v0);
                        af_left <= 17'(size_x0[16:0]);
                        dst <= 10'(x_start + size_x0[16:0] - 17'sd1);
                        state <= M4_APOP;
                    end
                    M4_APOP: begin
                        if (af_left == 17'd0)
                            next_sprite();
                        else if (af_over)
                            step_af();
                        else if (!dhit) begin
                            if (a_gnt) begin
                                fw_i <= 3'd1;
                                dcache_word <= af_byte_addr[15:2];
                                dcache_v <= 1'b0;
                            end
                            if (gnt_d) begin
                                dcache <= a_rdata;
                                dcache_v <= 1'b1;
                                fw_i <= '0;
                            end
                        end else
                            step_af();
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
                                     img_bytes, tex_x, attr[15:1],
                                     cur_byte_addr[17:16],
                                     cur_byte_addr[0],
                                     af_byte_addr[17:16],
                                     af_byte_addr[0], af_u, af_v};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
