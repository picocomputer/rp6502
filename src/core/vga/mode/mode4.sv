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

module mode4 (
    input logic clk,

    input logic start,
    input logic abort_i,
    input logic [15:0] attr,
    input logic [15:0] cfg,
    input logic [15:0] length,
    input logic [8:0] t_row,
    input logic [9:0] cw,

    output logic mode4_a_req,
    output logic [13:0] mode4_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    output logic [1:0] mode4_px_we,
    output logic [9:0] mode4_px_addr,
    output logic [31:0] mode4_px_data,

    output logic mode4_done
);

    logic affine;
    always_comb affine = attr[0];

    typedef enum logic [3:0] {
        M4_IDLE, M4_DECODE, M4_JUDGE, M4_META, M4_META2, M4_PIX,
        M4_ASETUP, M4_APOP, M4_ADRAIN
    } state_t;
    state_t state;

    logic [15:0] idx;
    logic [2:0] fw_i;

    /* XRAM answers the sprite stage two clocks after a grant, and each
     * grant is remembered as the list's or the row's so the word goes
     * where it was asked for. */
    logic gnt_d1, gnt_d, tag1, tag2;

    /* The list streams in ahead of the sprites through its own queue,
     * a word asked for whenever the row leaves the slot free and the
     * queue and the two clocks in flight can take one, up to the last
     * descriptor's word. A descriptor is read out of the queue's first
     * words and popped whole, so a list on a halfword boundary differs
     * only in where the window starts. */
    logic [31:0] dq[8];
    logic [3:0] dqn;
    logic [13:0] dfp, dend;
    logic [2:0] dw;   /* words to a descriptor */
    always_comb dw = affine ? 3'd5 : 3'd2;
    logic [191:0] dwin;
    always_comb dwin = {dq[5], dq[4], dq[3], dq[2], dq[1], dq[0]};
    logic [159:0] dsc;
    always_comb dsc = cfg[1] ? dwin[175:16] : dwin[159:0];
    logic dsc_v;
    always_comb dsc_v = dqn >= {1'b0, dw} + {3'd0, cfg[1]};
    logic [16:0] list_end;
    always_comb list_end = {1'b0, cfg}
        + (affine ? 17'(17'(length[12:0]) * 17'd20)
                  : {1'b0, length[12:0], 3'b000})
        - 17'd1;

    /* mode4_asprite_t is transform[6] followed by mode4_sprite_t's own
     * fields, so the two kinds differ only in where the common fields
     * start. */
    logic [63:0] dsc_c;
    always_comb dsc_c = affine ? dsc[159:96] : dsc[63:0];
    logic signed [15:0] dc_x, dc_y;
    logic [15:0] dc_sptr;
    logic [7:0] dc_log;
    logic dc_meta;
    logic signed [15:0] dc_t[6];
    always_comb begin
        for (int j = 0; j < 6; j++)
            dc_t[j] = dsc[16 * j+:16];
        dc_x = dsc_c[15:0];
        dc_y = dsc_c[31:16];
        dc_sptr = dsc_c[47:32];
        dc_log = dsc_c[55:48];
        dc_meta = dsc_c[63:56] != 8'h00;
    end

    /* Decoded once into registers, because the window moves on to the
     * next descriptor while this one draws, and off the window the whole
     * decode and the address adder would land in one clock. */
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
    logic [31:0] d_over;  /* the affine path's out-of-square mask */
    logic log_big;
    always_comb log_big = d_log[7:3] != 5'd0;
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
    /* The row's metadata is a word at that address, and the image's low
     * bits are its own, so off a word boundary it arrives in two. */
    logic [2:0] meta_n;
    always_comb meta_n = meta_addr[1:0] == 2'd0 ? 3'd1 : 3'd2;
    logic [31:0] meta_lo;
    logic meta_lo_v;
    logic [31:0] meta_w;
    always_comb meta_w = meta_addr[1:0] == 2'd0
        ? a_rdata
        : 32'({a_rdata, meta_lo} >> {meta_addr[1:0], 3'b000});
    /* Registered before the span arithmetic, which would otherwise run
     * from the XRAM's data port through two adders in the part of a
     * clock the port leaves. */
    logic [31:0] meta_q;

    /* The plain path streams the row's words through a short queue,
     * asked for whenever the queue and the two clocks in flight can
     * take one, up to the last texel's word: the head is the word the
     * texel is in, and a pair of texels is four bytes, so every pair
     * pops a word whatever the row's alignment. */
    logic [31:0] q[4];
    logic [2:0] qn;
    logic [13:0] fp;    /* the next word to ask for, once one has been */
    logic fp_v;
    logic signed [16:0] px_i;
    logic [9:0] dst;

    logic [17:0] tex_byte_addr;
    always_comb tex_byte_addr = {1'b0, d_sptr}
        + {(17'(row_texel) + 17'(px_i[15:0])), 1'b0};
    /* The last texel's high byte. */
    logic [17:0] end_byte;
    always_comb end_byte = {1'b0, d_sptr}
        + {(17'(row_texel) + 17'(span_end[15:0])), 1'b0} - 18'd1;
    logic [13:0] fetch_word;
    always_comb fetch_word = fp_v ? fp : tex_byte_addr[15:2];
    logic fetch_more;
    always_comb fetch_more = !fp_v || fp <= end_byte[15:2];

    /* A texel at byte 3 of a word, or the second of a pair past byte
     * 0, takes bytes from the next word too. */
    logic [1:0] tex_o;
    always_comb tex_o = tex_byte_addr[1:0];
    logic signed [16:0] left;
    always_comb left = span_end - px_i;
    logic need2, q_ok, emit, emit_pair, pop;
    always_comb begin
        need2 = tex_o == 2'd3 || (tex_o != 2'd0 && left > 17'sd1);
        q_ok = need2 ? qn >= 3'd2 : qn != 3'd0;
        emit = state == M4_PIX && q_ok && left > 17'sd0;
        emit_pair = emit && left > 17'sd1;
        pop = emit_pair;
    end
    logic [31:0] pair;
    always_comb pair = 32'({q[1], q[0]} >> {tex_o, 3'b000});

    /* The affine accumulators, the SIO interpolator's arithmetic: the
     * first sample is the span's last column and the scan runs backward,
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

    /* The span's first sample, its last column — four fixed-point
     * multiplies, every term wrapping mod 2^32 like the oracle's.
     *
     * (t << 8) * k and (t * k) << 8 agree on their low thirty-two bits,
     * and the second is a 16x18 multiply that fits one DSP multiplier
     * rather than a 32x32 multiply built from several. Twenty-four bits
     * of each product survive the shift, so the slice is exact and not a
     * rounding. */
    logic signed [17:0] kx;
    always_comb kx = 18'(tex_offs_x0) + size_x0 - 18'sd1;
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

    /* The affine path takes a column a clock: the accumulators step, a
     * sample inside the square asks for its word, or its two when the
     * texel straddles one, and the word lands two clocks on with the
     * column's place and byte carried beside the grant. */
    logic af_hi;   /* the straddle's second word is the one being asked for */
    logic af_straddle;
    always_comb af_straddle = af_byte_addr[1:0] == 2'b11;
    logic [13:0] af_hi_word;
    always_comb af_hi_word = af_byte_addr[15:2] + 14'd1;
    logic af_fetch;
    always_comb af_fetch = state == M4_APOP && af_left != 17'd0 && !af_over;
    logic [9:0] m_dst1, m_dst2;
    logic [1:0] m_o1, m_o2;
    logic m_hi1, m_hi2;
    logic [7:0] lo_byte;
    logic af_land, af_lo_land;
    always_comb begin
        af_land = (state == M4_APOP || state == M4_ADRAIN) && eng_land;
        af_lo_land = af_land && m_o2 == 2'd3 && !m_hi2;
    end
    /* Registered as it lands, so the texel's byte shift and the alpha
     * test do not run from the XRAM's data port in the part of a clock
     * the port leaves. */
    logic [31:0] af_word;
    logic [9:0] af_dst;
    logic [1:0] af_o;
    logic af_hi_q, af_wv;
    logic [15:0] af_texel;
    always_comb af_texel = af_hi_q
        ? {af_word[7:0], lo_byte}
        : 16'(af_word >> {af_o, 3'b000});

    /* The row's words come first; the list fills in behind. */
    logic eng_req, eng_gnt, eng_land, desc_req, desc_land, req_is_desc;
    logic dpop;
    logic [13:0] eng_addr;
    always_comb begin
        eng_land = gnt_d && !tag2;
        desc_land = gnt_d && tag2;
        dpop = state == M4_DECODE && dsc_v;
        eng_req = 1'b0;
        eng_addr = fetch_word;
        case (state)
            M4_META: begin
                eng_req = fw_i < meta_n;
                eng_addr = meta_addr[15:2] + {11'd0, fw_i};
            end
            /* Nothing is asked for on the way out of a row, because a
             * word landing after it would be taken for the next row. */
            M4_PIX:
                eng_req = fetch_more && left > 17'sd0
                    && qn + {2'd0, eng_land} + {2'd0, gnt_d1 && !tag1}
                       - {2'd0, pop} < 3'd4;
            M4_APOP: begin
                eng_req = af_fetch;
                eng_addr = af_hi ? af_hi_word : af_byte_addr[15:2];
            end
            default: ;
        endcase
        desc_req = state != M4_IDLE && dfp <= dend
            && dqn + {3'd0, desc_land} + {3'd0, gnt_d1 && tag1} < 4'd8;
        req_is_desc = !eng_req && desc_req;
        mode4_a_req = eng_req || desc_req;
        mode4_a_addr = eng_req ? eng_addr : dfp;
        eng_gnt = eng_req && a_gnt;
    end

    always_comb begin
        mode4_px_we = 2'b00;
        mode4_px_addr = dst;
        mode4_px_data = pair;
        if (state == M4_PIX) begin
            mode4_px_we[0] = emit && (meta_cont || pair[5]);
            mode4_px_we[1] = emit_pair && (meta_cont || pair[21]);
        end else if (af_wv) begin
            mode4_px_addr = af_dst;
            mode4_px_data = {16'd0, af_texel};
            mode4_px_we[0] = af_texel[5];
        end
    end

    task automatic next_sprite();
        qn <= '0;
        fp_v <= 1'b0;
        if (idx + 16'd1 == length) begin
            mode4_done <= 1'b1;
            state <= M4_IDLE;
        end else begin
            idx <= idx + 16'd1;
            state <= M4_DECODE;
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
        for (int j = 0; j < 8; j++)
            dq[j] = '0;
        dqn = '0;
        dfp = '0;
        dend = '0;
        tag1 = 1'b0;
        tag2 = 1'b0;
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
        fw_i = '0;
        meta_lo = '0;
        meta_lo_v = 1'b0;
        meta_q = '0;
        gnt_d1 = 1'b0;
        gnt_d = 1'b0;
        tex_x = '0;
        span_end = '0;
        meta_cont = 1'b0;
        row_texel = '0;
        for (int j = 0; j < 4; j++)
            q[j] = '0;
        qn = '0;
        fp = '0;
        fp_v = 1'b0;
        px_i = '0;
        dst = '0;
        af_u = '0;
        af_v = '0;
        af_left = '0;
        af_hi = 1'b0;
        m_dst1 = '0;
        m_dst2 = '0;
        m_o1 = '0;
        m_o2 = '0;
        m_hi1 = 1'b0;
        m_hi2 = 1'b0;
        lo_byte = '0;
        af_word = '0;
        af_dst = '0;
        af_o = '0;
        af_hi_q = 1'b0;
        af_wv = 1'b0;
        mode4_done = 1'b0;
    end
    always_ff @(posedge clk) begin
        gnt_d1 <= a_gnt;
        gnt_d <= gnt_d1;
        tag1 <= req_is_desc;
        tag2 <= tag1;
        m_dst1 <= dst;
        m_o1 <= af_byte_addr[1:0];
        m_hi1 <= af_hi;
        m_dst2 <= m_dst1;
        m_o2 <= m_o1;
        m_hi2 <= m_hi1;
        if (af_lo_land)
            lo_byte <= a_rdata[31:24];
        af_wv <= af_land && !af_lo_land;
        af_word <= a_rdata;
        af_dst <= m_dst2;
        af_o <= m_o2;
        af_hi_q <= m_hi2;
        mode4_done <= 1'b0;
        if (abort_i) begin
            /* sprite.sv has already counted this lost line; drop it. */
            state <= M4_IDLE;
        end else if (start) begin
            idx <= '0;
            qn <= '0;
            fp_v <= 1'b0;
            af_hi <= 1'b0;
            dqn <= '0;
            dfp <= cfg[15:2];
            dend <= list_end[15:2];
            if (length == 16'd0) begin
                mode4_done <= 1'b1;
                state <= M4_IDLE;
            end else
                state <= M4_DECODE;
        end else begin
            if (req_is_desc && a_gnt)
                dfp <= dfp + 14'd1;
            if (dpop) begin
                if (affine) begin
                    dq[0] <= dq[5];
                    dq[1] <= dq[6];
                    dq[2] <= dq[7];
                end else begin
                    dq[0] <= dq[2];
                    dq[1] <= dq[3];
                    dq[2] <= dq[4];
                    dq[3] <= dq[5];
                    dq[4] <= dq[6];
                    dq[5] <= dq[7];
                end
            end
            if (desc_land && state != M4_IDLE)
                dq[3'(dpop ? dqn - {1'b0, dw} : dqn)] <= a_rdata;
            dqn <= dqn + {3'd0, desc_land && state != M4_IDLE}
                - (dpop ? {1'b0, dw} : 4'd0);
            case (state)
                M4_IDLE: ;
                M4_DECODE: if (dsc_v) begin
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
                    span_end <= tex_offs_x0 + 17'(size_x0[16:0]);
                    meta_cont <= 1'b0;
                    row_texel <= 17'(17'(tex_offs_y[6:0]) << d_log[2:0]);
                    px_i <= tex_offs_x0;
                    dst <= 10'(x_start);
                    fw_i <= '0;
                    meta_lo_v <= 1'b0;
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
                    af_hi <= 1'b0;
                    state <= M4_APOP;
                end
                M4_APOP: begin
                    if (af_left == 17'd0)
                        state <= M4_ADRAIN;
                    else if (af_over)
                        step_af();
                    else if (eng_gnt) begin
                        if (af_straddle && !af_hi)
                            af_hi <= 1'b1;
                        else begin
                            af_hi <= 1'b0;
                            step_af();
                        end
                    end
                end
                /* The last words asked for land here, and the last
                 * texel is written the clock after it lands. */
                M4_ADRAIN:
                    if (!eng_land && !(gnt_d1 && !tag1))
                        next_sprite();
                M4_META: begin
                    if (eng_gnt)
                        fw_i <= fw_i + 3'd1;
                    if (eng_land && !meta_lo_v && meta_n == 3'd2) begin
                        meta_lo <= a_rdata;
                        meta_lo_v <= 1'b1;
                    end else if (eng_land) begin
                        meta_q <= meta_w;
                        meta_lo_v <= 1'b0;
                        fw_i <= '0;
                        state <= M4_META2;
                    end
                end
                M4_META2: begin
                    /* Narrow to the row's opaque span. The pixel loop
                     * below skips when the span comes up empty. */
                    if (17'($signed({2'd0, meta_q[30:16]})) > tex_x) begin
                        tex_x <= 17'({2'd0, meta_q[30:16]});
                        px_i <= 17'({2'd0, meta_q[30:16]});
                        dst <= 10'(17'(d_x) + 17'({2'd0, meta_q[30:16]}));
                    end
                    if (17'($signed({1'd0, meta_q[15:0]})) < span_end)
                        span_end <= 17'({1'd0, meta_q[15:0]});
                    meta_cont <= meta_q[31];
                    state <= M4_PIX;
                end
                M4_PIX: begin
                    if (eng_gnt) begin
                        fp <= fetch_word + 14'd1;
                        fp_v <= 1'b1;
                    end
                    if (pop) begin
                        q[0] <= q[1];
                        q[1] <= q[2];
                        q[2] <= q[3];
                    end
                    if (eng_land)
                        q[2'(pop ? qn - 3'd1 : qn)] <= a_rdata;
                    qn <= qn + {2'd0, eng_land} - {2'd0, pop};
                    if (left <= 17'sd0)
                        next_sprite();
                    else if (emit) begin
                        if (left <= 17'sd2)
                            next_sprite();
                        else begin
                            px_i <= px_i + 17'sd2;
                            dst <= dst + 10'd2;
                        end
                    end
                end
                default: state <= M4_IDLE;
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_mode4;
    always_comb unused_mode4 = ^{dwin[191:176], list_end[16],
                                     list_end[1:0], length[15:13],
                                     meta_addr[16],
                                     tex_byte_addr[17:16], size_x0[17],
                                     end_byte[17:16], end_byte[1:0],
                                     img_bytes, tex_x, attr[15:1],
                                     af_byte_addr[17:16], af_u, af_v};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
