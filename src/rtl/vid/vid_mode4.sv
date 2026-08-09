/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 4: an array of descriptors, each a power-of-two square of raw
 * RGB555 texels, walked in order so later sprites land on earlier ones.
 * A texel writes where its alpha bit is set; opacity metadata narrows a
 * row to its opaque span and skips the alpha test when the row is marked
 * continuous.
 */

module vid_mode4 (
    input logic clk,

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
        M4_IDLE, M4_NEXT, M4_DESC, M4_DECODE, M4_JUDGE, M4_META, M4_PIX,
        M4_ASETUP, M4_APOP
    } state_t;
    state_t state;

    logic [15:0] idx;

    /* A halfword shift register entered at the top, so the descriptor
     * ends flush against bit 159 wherever it started and the junk
     * halfword ahead of it falls out below the window.
     *
     * mode4_asprite_t is transform[6] followed by mode4_sprite_t's own
     * fields, so flush against the top puts both descriptors' position,
     * pointer, log and metadata flag in the same bits. */
    logic [159:0] gather;
    logic [15:0] hi_hold;
    logic hi_pend;
    logic [16:0] daddr;
    always_comb daddr = {1'b0, cfg}
        + (affine
           ? 17'(17'(idx[12:0]) * 17'd20)
           : {1'd0, idx[12:0], 3'b000});
    logic [2:0] fw_i, fw_n;
    logic [3:0] sh_c, sh_n;
    logic gnt_d;
    logic [15:0] sh_in;
    always_comb sh_in = gnt_d ? a_rdata[15:0] : hi_hold;
    logic signed [15:0] dc_x, dc_y;
    logic [15:0] dc_sptr;
    logic [7:0] dc_log;
    logic dc_meta;
    logic signed [15:0] dc_t[6];
    always_comb begin
        for (int j = 0; j < 6; j++)
            dc_t[j] = gather[16 * j+:16];
        dc_x = gather[111:96];
        dc_y = gather[127:112];
        dc_sptr = gather[143:128];
        dc_log = gather[151:144];
        dc_meta = gather[159:152] != 8'h00;
    end

    /* Decoded once into registers: off the gathered words the whole
     * decode and the address adder land in one clock, and the sprite's
     * address is what the XRAM arbiter selects on, so every other
     * engine's grant arrives through that cone too. */
    logic signed [15:0] d_x, d_y;
    logic [15:0] d_sptr;
    logic [7:0] d_log;
    logic d_meta;
    logic signed [15:0] d_t[6];

    /* Registers rather than recomputed: each stood in front of the
     * address adder, putting a variable shift and an add between a
     * register and the XRAM's address port. */
    logic [7:0] size;
    logic [6:0] d_mask;   /* the texel index mask, (1 << log) - 1 */
    logic [31:0] d_over;  /* the affine walk's out-of-square mask */
    logic log_big;
    always_comb log_big = d_log[7:3] != 5'd0;
    /* The oracle wraps byte_size in 32 bits, so a plain sprite with log
     * 16..31 and no metadata wraps to zero and passes both guards. Its
     * zero row is defined C; deeper rows read past XRAM on the host, and
     * the RTL re-reads row zero instead. */
    logic log_wrap;
    always_comb log_wrap = !affine && !d_meta
        && d_log >= 8'd16 && d_log < 8'd32;
    logic signed [17:0] size_x_eff;
    always_comb size_x_eff = log_wrap
        ? 18'($signed({8'd0, cw})) - 18'(x_start)
        : size_x0;
    logic [16:0] img_bytes;
    logic [17:0] byte_size;
    always_comb byte_size = {1'b0, img_bytes}
        + (d_meta ? 18'({size, 2'b00}) : 18'd0);

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

    logic signed [16:0] tex_x, span_end;
    logic meta_cont;
    logic [16:0] row_texel;  /* texel index of the row's first column */

    logic [16:0] meta_addr;
    always_comb meta_addr = {1'b0, d_sptr} + img_bytes[16:0]
        + {8'd0, tex_offs_y[6:0], 2'b00};

    /* A word carries two texels, so the plain walk has a spare clock and
     * spends it asking for the next word — crossing a word boundary
     * without stopping. Only the plain path prefetches; the affine
     * walk's addresses are not sequential. */
    logic [31:0] dcache;
    logic [13:0] dcache_word;
    logic dcache_v;
    logic [31:0] pre_data;
    logic [13:0] pre_word;
    logic pre_v;     /* the next word is here */
    logic pre_pend;  /* ...or it has been asked for */
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
    always_comb af_over = ((af_u | af_v) & d_over) != 32'd0;
    logic [17:0] af_byte_addr;
    always_comb begin
        logic [6:0] ui, vi;
        ui = af_u[22:16] & d_mask;
        vi = af_v[22:16] & d_mask;
        af_byte_addr = {2'b0, d_sptr}
            + {10'd0, ui, 1'b0}
            + (18'({11'd0, vi}) << (d_log[2:0] + 4'd1));
    end

    /* The span's first sample, one past its end — four fixed-point
     * multiplies, every term wrapping mod 2^32 like the oracle's.
     *
     * (t << 8) * k and (t * k) << 8 agree on their low thirty-two bits,
     * and the second is a 16x18 the fabric has a DSP for rather than a
     * 32-square it must build. Twenty-four bits of each product survive
     * the shift, so the slice is exact and not a rounding. */
    logic signed [17:0] kx;
    always_comb kx = 18'(tex_offs_x0) + size_x0;
    /* Registered, so the multiply and the sum after it are not one
     * clock's work. Together in one hop this was the longest path in the
     * machine: a size bit through the width adder, a 24-bit multiply,
     * then a three-way 32-bit add. */
    logic signed [23:0] pu_x, pu_y, pv_x, pv_y;
    always_ff @(posedge clk) begin
        pu_x <= 24'(d_t[0] * kx);
        pu_y <= 24'(d_t[1] * tex_offs_y);
        pv_x <= 24'(d_t[3] * kx);
        pv_y <= 24'(d_t[4] * tex_offs_y);
    end
    logic signed [31:0] af_u0, af_v0;
    always_comb begin
        af_u0 = 32'({pu_x, 8'd0}) + 32'({pu_y, 8'd0})
                + {{8{d_t[2][15]}}, d_t[2], 8'd0};
        af_v0 = 32'({pv_x, 8'd0}) + 32'({pv_y, 8'd0})
                + {{8{d_t[5][15]}}, d_t[5], 8'd0};
    end

    logic [13:0] pre_next;
    logic pre_want;
    logic [17:0] cur_byte_addr;
    always_comb cur_byte_addr =
        state == M4_APOP ? af_byte_addr : tex_byte_addr;
    /* A prefetch hit emits and promotes in the same clock; promoting
     * first would give the boundary back the clock it was there to
     * save. */
    logic dhit, pre_hit, hit_any;
    always_comb begin
        dhit = dcache_v && dcache_word == cur_byte_addr[15:2];
        pre_hit = state == M4_PIX && pre_v
            && pre_word == cur_byte_addr[15:2];
        hit_any = dhit || pre_hit;
    end
    logic [31:0] hit_data;
    always_comb hit_data = dhit ? dcache : pre_data;
    logic [15:0] texel;
    always_comb texel = cur_byte_addr[1] ? hit_data[31:16]
                                         : hit_data[15:0];
    always_comb pre_next = (pre_hit ? pre_word : dcache_word) + 14'd1;
    always_comb pre_want = state == M4_PIX && hit_any && dcache_v
        && !pre_v && !pre_pend;

    always_comb begin
        vid_mode4_a_req = 1'b0;
        vid_mode4_a_addr = daddr[15:2] + {11'd0, fw_i};
        case (state)
            /* One word in flight: the half held back has to shift before the
             * next word's low half arrives, and dropping the request for the
             * grant's own clock is what spaces them. */
            M4_DESC: vid_mode4_a_req = fw_i < fw_n && !gnt_d;
            M4_META: begin
                vid_mode4_a_req = fw_i == 3'd0;
                vid_mode4_a_addr = meta_addr[15:2];
            end
            M4_PIX: begin
                /* A prefetch of this word may still be in flight, and a
                 * duplicate miss fetch would land on a clock the promote
                 * path already covers — leaving fw_i raised and the
                 * request line silent for the rest of the walk. */
                if (!dhit
                    && !((pre_v || pre_pend)
                         && pre_word == cur_byte_addr[15:2]))
                begin
                    vid_mode4_a_req = fw_i == 3'd0;
                    vid_mode4_a_addr = tex_byte_addr[15:2];
                end else if (pre_want) begin
                    vid_mode4_a_req = 1'b1;
                    vid_mode4_a_addr = pre_next;
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
        if (state == M4_PIX && hit_any && px_i < span_end)
            vid_mode4_px_we = meta_cont || texel[5];
        else if (state == M4_APOP && !af_over && dhit
                 && af_left != 17'd0)
            vid_mode4_px_we = texel[5];
    end

    task automatic next_sprite();
        /* Whatever was read ahead belonged to the sprite just finished. */
        pre_v <= 1'b0;
        pre_pend <= 1'b0;
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

    initial begin
        state = M4_IDLE;
        idx = '0;
        gather = '0;
        d_x = '0;
        d_y = '0;
        d_sptr = '0;
        d_log = '0;
        d_meta = 1'b0;
        size = '0;
        img_bytes = '0;
        d_mask = '0;
        d_over = '0;
        for (int j = 0; j < 6; j++)
            d_t[j] = '0;
        hi_hold = '0;
        hi_pend = 1'b0;
        fw_i = '0;
        fw_n = '0;
        sh_c = '0;
        sh_n = '0;
        gnt_d = 1'b0;
        tex_x = '0;
        span_end = '0;
        meta_cont = 1'b0;
        row_texel = '0;
        dcache = '0;
        dcache_word = '0;
        dcache_v = 1'b0;
        pre_data = '0;
        pre_word = '0;
        pre_v = 1'b0;
        pre_pend = 1'b0;
        px_i = '0;
        dst = '0;
        af_u = '0;
        af_v = '0;
        af_left = '0;
        vid_mode4_done = 1'b0;
    end
    always_ff @(posedge clk) begin
        gnt_d <= a_gnt;
        vid_mode4_done <= 1'b0;
        if (abort_i) begin
            /* A lost race: the scaffold counted it; drop the line. */
            state <= M4_IDLE;
        end else if (start) begin
            idx <= '0;
            dcache_v <= 1'b0;
            pre_v <= 1'b0;
            pre_pend <= 1'b0;
            if (length == 16'd0) begin
                vid_mode4_done <= 1'b1;
                state <= M4_IDLE;
            end else
                state <= M4_NEXT;
        end else begin
            case (state)
                M4_IDLE: ;
                M4_NEXT: begin
                    /* cfg[1] rather than daddr[1]: both strides are multiples
                     * of four, so the array's alignment is every descriptor's,
                     * and the address adder stays out of it. */
                    fw_i <= '0;
                    sh_c <= '0;
                    hi_pend <= 1'b0;
                    fw_n <= affine ? (cfg[1] ? 3'd6 : 3'd5)
                                   : (cfg[1] ? 3'd3 : 3'd2);
                    sh_n <= affine ? (cfg[1] ? 4'd11 : 4'd10)
                                   : (cfg[1] ? 4'd5 : 4'd4);
                    state <= M4_DESC;
                end
                M4_DESC: begin
                    if (a_gnt)
                        fw_i <= fw_i + 3'd1;
                    hi_pend <= gnt_d;
                    if (gnt_d)
                        hi_hold <= a_rdata[31:16];
                    if (gnt_d || hi_pend) begin
                        gather <= {sh_in, gather[159:16]};
                        sh_c <= sh_c + 4'd1;
                        if (sh_c + 4'd1 == sh_n)
                            state <= M4_DECODE;
                    end
                end
                M4_DECODE: begin
                    d_x <= dc_x;
                    d_y <= dc_y;
                    d_sptr <= dc_sptr;
                    d_log <= dc_log;
                    d_meta <= dc_meta;
                    size <= dc_log[7:3] == 5'd0
                        ? 8'(8'd1 << dc_log[2:0]) : 8'd0;
                    img_bytes <= 17'(17'd2 << {13'd0, dc_log[2:0], 1'b0});
                    d_mask <= 7'((8'd1 << dc_log[2:0]) - 8'd1);
                    d_over <= 32'hFFFF0000 << dc_log[2:0];
                    for (int j = 0; j < 6; j++)
                        d_t[j] <= dc_t[j];
                    state <= M4_JUDGE;
                end
                M4_JUDGE: begin
                    tex_x <= tex_offs_x0;
                    span_end <= tex_offs_x0 + 17'(size_x_eff[16:0]);
                    meta_cont <= 1'b0;
                    row_texel <= log_wrap ? 17'd0
                        : 17'(17'(tex_offs_y[6:0]) << d_log[2:0]);
                    px_i <= tex_offs_x0;
                    dst <= 10'(x_start);
                    fw_i <= '0;
                    if (log_wrap
                        ? (tex_offs_y < 0 || size_x_eff < 18'sd1)
                        : (log_big
                           || byte_size > 18'h10000
                           || {2'b0, d_sptr} > 18'h10000 - byte_size
                           || tex_offs_y < 0
                           || tex_offs_y
                               >= 17'($signed({9'd0, size}))
                           || size_x0 < 18'sd1))
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
                    /* The prefetch's own answer, told apart from
                     * the miss fetch's by which one is pending —
                     * only ever one is outstanding, because the
                     * request logic asks for one or the other. */
                    if (pre_pend && gnt_d) begin
                        pre_data <= a_rdata;
                        pre_v <= 1'b1;
                        pre_pend <= 1'b0;
                    end else if (pre_want && a_gnt) begin
                        pre_word <= pre_next;
                        pre_pend <= 1'b1;
                    end
                    if (px_i >= span_end)
                        next_sprite();
                    else if (!hit_any) begin
                        if (a_gnt && !pre_want) begin
                            fw_i <= 3'd1;
                            dcache_word <= tex_byte_addr[15:2];
                            dcache_v <= 1'b0;
                        end
                        if (gnt_d && !pre_pend) begin
                            dcache <= a_rdata;
                            dcache_v <= 1'b1;
                            fw_i <= '0;
                        end
                    end else begin
                        if (pre_hit) begin
                            dcache <= pre_data;
                            dcache_word <= pre_word;
                            dcache_v <= 1'b1;
                            pre_v <= 1'b0;
                        end
                        px_i <= px_i + 17'sd1;
                        dst <= dst + 10'd1;
                    end
                end
                default: state <= M4_IDLE;
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_mode4;
    always_comb unused_vid_mode4 = ^{gather, daddr[16], daddr[1:0],
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
