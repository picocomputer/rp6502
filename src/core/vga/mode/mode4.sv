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
    input logic [8:0] t_row,
    input logic [9:0] cw,

    output logic mode4_a_req,
    output logic [13:0] mode4_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    /* The descriptor list queue sprite.sv shares between the engines:
     * what it needs from this one, and the descriptor at its head. */
    output logic mode4_lq_active,
    output logic [3:0] mode4_lq_size,
    output logic mode4_lq_pop,
    output logic mode4_lq_gnt,
    input logic lq_req,
    input logic [13:0] lq_addr,
    input logic [159:0] lq_dsc,
    input logic lq_v,
    input logic lq_end,

    /* The row word queue sprite.sv shares between the engines: what it
     * needs from this one, and the words it holds. */
    output logic mode4_rq_run,
    output logic [13:0] mode4_rq_first,
    output logic [13:0] mode4_rq_last,
    output logic mode4_rq_want,
    output logic mode4_rq_pop,
    output logic mode4_rq_pop_room,
    input logic rq_req,
    input logic [13:0] rq_addr,
    input logic [31:0] rq_q0,
    input logic [31:0] rq_q1,
    input logic [2:0] rq_n,

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

    logic [2:0] fw_i;

    /* XRAM answers the sprite stage two clocks after a grant. These follow
     * the engine's own metadata and affine words; the queues follow
     * theirs. */
    logic eng_d1, eng_d2;

    /* The list streams in ahead of the sprites through its own queue. */
    always_comb begin
        mode4_lq_active = state != M4_IDLE;
        mode4_lq_size = affine ? 4'd10 : 4'd4;
        mode4_lq_pop = dpop;
        mode4_lq_gnt = req_is_desc && a_gnt;
    end

    /* mode4_asprite_t is transform[6] followed by mode4_sprite_t's own
     * fields, so the two kinds differ only in where the common fields
     * start. */
    logic [63:0] dsc_c;
    always_comb dsc_c = affine ? lq_dsc[159:96] : lq_dsc[63:0];
    logic signed [15:0] dc_x, dc_y;
    logic [15:0] dc_sptr;
    logic [7:0] dc_log;
    logic dc_meta;
    logic signed [15:0] dc_t[6];
    always_comb begin
        for (int j = 0; j < 6; j++)
            dc_t[j] = lq_dsc[16 * j+:16];
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
    logic log_big;
    always_comb log_big = d_log[7:3] != 5'd0;
    logic [16:0] img_bytes;

    logic signed [16:0] tex_offs_y;
    always_comb tex_offs_y = 17'($signed({8'd0, t_row}) - 17'(d_y));
    logic signed [16:0] x_start;
    always_comb x_start = d_x < 0 ? 17'sd0 : 17'(d_x);
    /* A drawn sprite is at most 128 wide, so the columns cut off its left
     * edge, the span and the affine walk fit eight bits. The span's start
     * can come from the row's metadata, anywhere in fifteen. */
    logic [6:0] tex_offs_x0;
    always_comb tex_offs_x0 = d_x < 0 ? 7'(-d_x[6:0]) : 7'd0;
    logic signed [17:0] span_rhs;
    always_comb span_rhs = 18'(d_x) + 18'($signed({10'd0, size}));
    logic signed [17:0] size_x0;
    always_comb size_x0 =
        (span_rhs < 18'($signed({8'd0, cw})) ? span_rhs
                                             : 18'($signed({8'd0, cw})))
        - 18'(x_start);

    logic [7:0] span_end;
    logic meta_cont;
    logic [16:0] row_texel;  /* texel index of the row's first column */

    /* The row's metadata is a word at that address, and the image's low
     * bits are its own, so off a word boundary it arrives in two. The
     * second word's address is the first's plus three bytes, which the
     * adder's free low bits carry. */
    logic [16:0] meta_addr;
    always_comb meta_addr = {1'b0, d_sptr} + img_bytes[16:0]
        + {8'd0, tex_offs_y[6:0], fw_i[0], fw_i[0]};
    logic [1:0] meta_o;
    always_comb meta_o = d_sptr[1:0] + img_bytes[1:0];
    logic [2:0] meta_n;
    always_comb meta_n = meta_o == 2'd0 ? 3'd1 : 3'd2;
    logic [31:0] meta_lo;
    logic meta_lo_v;
    logic [31:0] meta_w;
    always_comb meta_w = meta_o == 2'd0
        ? a_rdata
        : 32'({a_rdata, meta_lo} >> {meta_o, 3'b000});
    /* Registered before the span arithmetic, which would otherwise run
     * from the XRAM's data port through two adders in the part of a
     * clock the port leaves. */
    logic [31:0] meta_q;

    logic [14:0] px_i;
    logic [9:0] dst;

    logic [17:0] tex_byte_addr;
    always_comb tex_byte_addr = {1'b0, d_sptr}
        + {(17'(row_texel) + 17'(px_i)), 1'b0};
    /* The last texel's high byte. */
    logic [17:0] end_byte;
    always_comb end_byte = {1'b0, d_sptr}
        + {(17'(row_texel) + 17'(span_end)), 1'b0} - 18'd1;

    /* The plain path streams the row's words through the queue. A pair of
     * texels is four bytes, so every pair pops a word whatever the row's
     * alignment. */
    always_comb begin
        mode4_rq_run = state == M4_PIX;
        mode4_rq_first = tex_byte_addr[15:2];
        mode4_rq_last = end_byte[15:2];
        mode4_rq_want = left > 16'sd0;
        mode4_rq_pop = pop;
        mode4_rq_pop_room = pop;
    end

    /* A texel at byte 3 of a word, or the second of a pair past byte
     * 0, takes bytes from the next word too. */
    logic [1:0] tex_o;
    always_comb tex_o = tex_byte_addr[1:0];
    logic signed [15:0] left;
    always_comb left = $signed({8'd0, span_end}) - $signed({1'b0, px_i});
    logic need2, q_ok, emit, emit_pair, pop;
    always_comb begin
        need2 = tex_o == 2'd3 || (tex_o != 2'd0 && left > 16'sd1);
        q_ok = need2 ? rq_n >= 3'd2 : rq_n != 3'd0;
        emit = state == M4_PIX && q_ok && left > 16'sd0;
        emit_pair = emit && left > 16'sd1;
        pop = emit_pair;
    end
    logic [31:0] pair;
    always_comb pair = 32'({rq_q1, rq_q0} >> {tex_o, 3'b000});

    /* The affine accumulators, the SIO interpolator's arithmetic: the
     * first sample is the span's last column and the scan runs backward,
     * subtracting a00 and a10 in wrapping thirty-two bits — the exact
     * uint32 stream the oracle's software interpolator produces. */
    logic signed [31:0] af_a00, af_a10;
    always_comb af_a00 = {{8{d_t[0][15]}}, d_t[0], 8'd0};
    always_comb af_a10 = {{8{d_t[3][15]}}, d_t[3], 8'd0};
    logic [31:0] af_u, af_v;
    logic [7:0] af_left;    /* pops remaining */
    logic af_over;
    always_comb af_over = ((af_u[31:16] | af_v[31:16])
                           & {9'h1FF, ~d_mask}) != 16'd0;
    logic [17:0] af_byte_addr;
    always_comb begin
        logic [6:0] ui, vi;
        ui = af_u[22:16] & d_mask;
        vi = af_v[22:16] & d_mask;
        af_byte_addr = {2'b0, d_sptr}
            + {10'd0, ui, af_hi}
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
    always_comb kx = $signed({11'd0, tex_offs_x0}) + size_x0 - 18'sd1;
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
    /* The straddle's second word is the one being asked for. It is the
     * first's plus a byte, which the texel address's free low bit adds. */
    logic af_hi;
    logic af_straddle;
    always_comb af_straddle = af_byte_addr[1:0] == 2'b11;
    logic af_fetch;
    always_comb af_fetch = state == M4_APOP && af_left != 8'd0 && !af_over;
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
    logic [1:0] af_s;   /* the texel's byte in {af_word, lo_byte} */
    logic af_wv;
    logic [15:0] af_texel;
    always_comb af_texel = 16'({af_word, lo_byte} >> {af_s, 3'b000});

    /* The row's words come first; the list fills in behind. */
    logic eng_req, eng_gnt, eng_land, req_is_desc;
    logic dpop;
    logic [13:0] eng_addr;
    always_comb begin
        eng_land = eng_d2;
        dpop = state == M4_DECODE && lq_v;
        eng_req = 1'b0;
        eng_addr = rq_addr;
        case (state)
            M4_META: begin
                eng_req = fw_i < meta_n;
                eng_addr = meta_addr[15:2];
            end
            M4_PIX:
                eng_req = rq_req;
            M4_APOP: begin
                eng_req = af_fetch;
                eng_addr = af_byte_addr[15:2];
            end
            default: ;
        endcase
        req_is_desc = !eng_req && lq_req;
        mode4_a_req = eng_req || lq_req;
        mode4_a_addr = eng_req ? eng_addr : lq_addr;
        eng_gnt = eng_req && a_gnt;
    end

    /* An affine texel is written only outside the plain path's pixel
     * state, and never as the pair's second. */
    always_comb begin
        mode4_px_we[0] = af_wv ? af_texel[5] : emit && (meta_cont || pair[5]);
        mode4_px_we[1] = emit_pair && (meta_cont || pair[21]);
        mode4_px_addr = af_wv ? af_dst : dst;
        mode4_px_data = {pair[31:16], af_wv ? af_texel : pair[15:0]};
    end

    task automatic next_sprite();
        if (lq_end) begin
            mode4_done <= 1'b1;
            state <= M4_IDLE;
        end else
            state <= M4_DECODE;
    endtask

    task automatic step_af();
        af_u <= af_u - 32'(af_a00);
        af_v <= af_v - 32'(af_a10);
        dst <= dst - 10'd1;
        af_left <= af_left - 8'd1;
    endtask

    initial begin
        state = M4_IDLE;
        d_x = '0;
        d_y = '0;
        d_sptr = '0;
        d_log = '0;
        d_meta = 1'b0;
        size = '0;
        img_bytes = '0;
        d_mask = '0;
        for (int j = 0; j < 6; j++)
            d_t[j] = '0;
        fw_i = '0;
        meta_lo = '0;
        meta_lo_v = 1'b0;
        meta_q = '0;
        eng_d1 = 1'b0;
        eng_d2 = 1'b0;
        span_end = '0;
        meta_cont = 1'b0;
        row_texel = '0;
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
        af_s = '0;
        af_wv = 1'b0;
        mode4_done = 1'b0;
    end
    always_ff @(posedge clk) begin
        eng_d1 <= eng_gnt;
        eng_d2 <= eng_d1;
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
        af_s <= m_hi2 ? 2'd0 : m_o2 + 2'd1;
        mode4_done <= 1'b0;
        if (abort_i) begin
            /* sprite.sv has already counted this lost line; drop it. */
            state <= M4_IDLE;
        end else if (start) begin
            af_hi <= 1'b0;
            state <= M4_DECODE;
        end else begin
            case (state)
                M4_IDLE: ;
                M4_DECODE: if (lq_v) begin
                    d_x <= dc_x;
                    d_y <= dc_y;
                    d_sptr <= dc_sptr;
                    d_log <= dc_log;
                    d_meta <= dc_meta;
                    size <= dc_log[7:3] == 5'd0
                        ? 8'(8'd1 << dc_log[2:0]) : 8'd0;
                    img_bytes <= 17'(17'd2 << {13'd0, dc_log[2:0], 1'b0});
                    d_mask <= 7'((8'd1 << dc_log[2:0]) - 8'd1);
                    for (int j = 0; j < 6; j++)
                        d_t[j] <= dc_t[j];
                    state <= M4_JUDGE;
                end
                M4_JUDGE: begin
                    span_end <= {1'b0, tex_offs_x0} + size_x0[7:0];
                    meta_cont <= 1'b0;
                    row_texel <= 17'(17'(tex_offs_y[6:0]) << d_log[2:0]);
                    px_i <= {8'd0, tex_offs_x0};
                    dst <= 10'(x_start);
                    fw_i <= '0;
                    meta_lo_v <= 1'b0;
                    if (log_big
                        || {1'b0, d_sptr} + img_bytes
                           + (d_meta ? 17'({size, 2'b00}) : 17'd0)
                           > 17'h10000
                        || |tex_offs_y[16:7]
                        || |(tex_offs_y[6:0] & ~d_mask)
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
                    af_left <= size_x0[7:0];
                    dst <= 10'(x_start + size_x0[16:0] - 17'sd1);
                    af_hi <= 1'b0;
                    state <= M4_APOP;
                end
                M4_APOP: begin
                    if (af_left == 8'd0)
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
                    if (!eng_d2 && !eng_d1)
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
                    if (meta_q[30:16] > px_i) begin
                        px_i <= meta_q[30:16];
                        dst <= 10'(17'(d_x) + 17'({2'd0, meta_q[30:16]}));
                    end
                    if (meta_q[15:0] < {8'd0, span_end})
                        span_end <= meta_q[7:0];
                    meta_cont <= meta_q[31];
                    state <= M4_PIX;
                end
                M4_PIX: begin
                    if (left <= 16'sd0)
                        next_sprite();
                    else if (emit) begin
                        if (left <= 16'sd2)
                            next_sprite();
                        else begin
                            px_i <= px_i + 15'd2;
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
    always_comb unused_mode4 = ^{meta_addr[16], meta_addr[1:0],
                                     tex_byte_addr[17:16], size_x0[17],
                                     end_byte[17:16], end_byte[1:0],
                                     img_bytes, attr[15:1],
                                     af_byte_addr[17:16], af_u, af_v};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
