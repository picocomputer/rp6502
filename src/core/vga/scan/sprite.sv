/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The sprite stage: three slots, one per plane, each with a ping-pong
 * line buffer of its own (sbuf). Nothing here waits on a fill and nothing
 * clears a buffer, because the scan side erases behind the pixel being
 * displayed, so a slot scans out its written pixels over transparent
 * zeros and the engines can start as soon as the slots are decoded.
 * Sprites paint in list order within a slot; across slots, compose
 * stacks the planes.
 */

module sprite (
    input logic clk,

    input logic [9:0] v,
    input logic [9:0] h,
    input logic px_last,
    input logic line_start,

    input logic [9:0] cw,
    input logic [9:0] ch,

    output logic [12:0] sprite_s_idx,
    input logic [31:0] s_data,

    output logic [16:0] sprite_pix[3],

    /* Counts the lines whose sprites did not finish before the end of
     * the line. Such a line shows whatever was painted. */
    output logic [15:0] sprite_overrun /*verilator public_flat_rd*/,

    output logic sprite_a_req,
    output logic [13:0] sprite_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata
);

    logic [9:0] t;
    logic render_now;
    always_comb render_now = t < ch;
    logic [8:0] t_row;
    always_comb t_row = t[8:0];

    typedef enum logic [1:0] {
        SP_IDLE, SP_SLOT, SP_PLAN, SP_RUN
    } state_t;
    state_t state;

    /* The three sprite slots, read at line start: an index goes out each
     * clock and the word answers on the next. */
    logic [31:0] slot_entry[3];
    logic [31:0] slot_cfg[3];
    logic [2:0] s_n, s_cap;
    logic s_cap_v;
    always_comb sprite_s_idx = {t_row, s_n[2:1], 1'b1, s_n[0]};

    logic [1:0] p;

    logic m5_start;
    logic m5_a_req;
    logic [13:0] m5_a_addr;
    logic m5_px_we;
    logic [9:0] m5_px_addr;
    logic [15:0] m5_px_data;
    logic m5_done;

    logic pal_lookup, pal_xram, pal_one_bpp;
    logic [15:0] pal_base;
    logic [7:0] pal_idx;
    logic pal_hit;
    logic [15:0] pal_q;
    logic pc_req;
    logic [13:0] pc_addr;
    logic pc_gnt, pc_rdy;
    initial pc_rdy = 1'b0;
    always_ff @(posedge clk)
        pc_rdy <= pc_gnt;
    /* verilator lint_off PINCONNECTEMPTY */
    palcache palcache (
        .clk(clk),
        .lookup(pal_lookup),
        .xram(pal_xram),
        .one_bpp(pal_one_bpp),
        .base(pal_base),
        .idx_a(pal_idx),
        .idx_b(8'd0),
        .need_b(1'b0),
        .palcache_qa(pal_q),
        .palcache_qb(),
        .palcache_hit(pal_hit),
        .palcache_req(pc_req),
        .palcache_addr(pc_addr),
        .fill_gnt(pc_gnt),
        .fill_rdy(pc_rdy),
        .a_rdata(a_rdata),
        .flush(line_start)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    mode5 mode5 (
        .clk(clk),
        .start(m5_start),
        .abort_i(line_start),
        .attr(slot_entry[p][15:0]),
        .cfg(slot_cfg[p][15:0]),
        .length(slot_cfg[p][31:16]),
        .t_row(t_row),
        .cw(cw),
        .mode5_a_req(m5_a_req),
        .mode5_a_addr(m5_a_addr),
        .a_gnt(a_gnt && !pc_req),
        .a_rdata(a_rdata),
        .mode5_px_we(m5_px_we),
        .mode5_px_addr(m5_px_addr),
        .mode5_px_data(m5_px_data),
        .mode5_pal_lookup(pal_lookup),
        .mode5_pal_xram(pal_xram),
        .mode5_pal_one_bpp(pal_one_bpp),
        .mode5_pal_base(pal_base),
        .mode5_pal_idx(pal_idx),
        .pal_hit(pal_hit),
        .pal_q(pal_q),
        .mode5_done(m5_done)
    );
    logic m4_start;
    logic m4_a_req;
    logic [13:0] m4_a_addr;
    logic m4_px_we;
    logic [9:0] m4_px_addr;
    logic [15:0] m4_px_data;
    logic m4_done;
    mode4 mode4 (
        .clk(clk),
        .start(m4_start),
        .abort_i(line_start),
        .attr(slot_entry[p][15:0]),
        .cfg(slot_cfg[p][15:0]),
        .length(slot_cfg[p][31:16]),
        .t_row(t_row),
        .cw(cw),
        .mode4_a_req(m4_a_req),
        .mode4_a_addr(m4_a_addr),
        .a_gnt(a_gnt),
        .a_rdata(a_rdata),
        .mode4_px_we(m4_px_we),
        .mode4_px_addr(m4_px_addr),
        .mode4_px_data(m4_px_data),
        .mode4_done(m4_done)
    );

    logic sp_is4;
    always_comb sp_is4 = slot_entry[p][18:16] == 3'd4;

    /* Latched when the plane is planned, because sp_is4 reads
     * slot_entry[p][18:16] through a three-way mux, which would
     * otherwise stand in both the pixel port's data path and the XRAM
     * arbitration for a bit that cannot change while the engine runs. */
    logic run4;

    /* The cache's fill preempts mode 5's own requests. A palette lookup
     * only happens while the index word is in hand, which is exactly when
     * mode 5 asks to prefetch the next one, so a grant arriving while the
     * cache is asking belongs to the cache and mode 5's a_gnt is masked
     * to say so. */
    always_comb begin
        sprite_a_req = state == SP_RUN
            && (run4 ? m4_a_req : (pc_req || m5_a_req));
        sprite_a_addr = run4 ? m4_a_addr
            : pc_req ? pc_addr : m5_a_addr;
        pc_gnt = a_gnt && pc_req && state == SP_RUN && !run4;
    end

    logic sp_en;
    always_comb sp_en = slot_entry[p][31]
        && (slot_entry[p][18:16] == 3'd5 || sp_is4);

    logic wr_bank;
    logic flip_next;

    /* The bank flip lands on h==0's first clock, so only the pixel-0
     * read at the end of h==799 has to take the bank the flip is about
     * to turn into the scan bank. The erase runs the width of the scan
     * rather than stopping at cw, so a switch to a narrower canvas
     * cannot strand pixels. */
    logic [10:0] sb_rd;
    always_comb sb_rd = h == 10'd799
        ? {flip_next ? wr_bank : !wr_bank, 10'd0}
        : {!wr_bank, h + 10'd1};
    logic sb_we;
    always_comb sb_we = !px_last;

    logic eng_we;
    logic [9:0] eng_addr;
    logic [16:0] eng_data;
    always_comb begin
        eng_we = state == SP_RUN && (run4 ? m4_px_we : m5_px_we);
        eng_addr = run4 ? m4_px_addr : m5_px_addr;
        eng_data = {1'b1, run4 ? m4_px_data : m5_px_data};
    end

    genvar gi;
    generate
        for (gi = 0; gi < 3; gi++) begin : gen_sbuf
            sbuf sbuf (
                .clk(clk),
                .wr_bank(wr_bank),
                .a_we(eng_we && p == 2'(gi)),
                .a_addr(eng_addr),
                .a_data(eng_data),
                .sc_we(sb_we),
                .sc_addr(h - 10'd1),
                .rd_en(px_last),
                .rd_addr(sb_rd[9:0]),
                .rd_bank(sb_rd[10]),
                .sbuf_pix(sprite_pix[gi])
            );
        end
    endgenerate

    task automatic next_plane();
        if (p == 2'd2)
            state <= SP_IDLE;
        else begin
            p <= p + 2'd1;
            state <= SP_PLAN;
        end
    endtask

    initial begin
        state = SP_IDLE;
        t = '0;
        slot_entry[0] = '0;
        slot_entry[1] = '0;
        slot_entry[2] = '0;
        slot_cfg[0] = '0;
        slot_cfg[1] = '0;
        slot_cfg[2] = '0;
        s_n = '0;
        s_cap = '0;
        s_cap_v = 1'b0;
        p = '0;
        m4_start = 1'b0;
        m5_start = 1'b0;
        run4 = 1'b0;
        wr_bank = 1'b0;
        flip_next = 1'b0;
        sprite_overrun = '0;
    end
    always_ff @(posedge clk) begin
        m4_start <= 1'b0;
        m5_start <= 1'b0;
        if (h == 10'd799 && state != SP_IDLE) begin
            /* Count the lost line once and drop it; the engines abort
             * at the next line_start. */
            sprite_overrun <= sprite_overrun
                + 16'd1;
            state <= SP_IDLE;
        end else if (line_start) begin
            t <= v == 10'd524 ? 10'd0 : v + 10'd1;
            if (flip_next)
                wr_bank <= !wr_bank;
            flip_next <= 1'b0;
            s_n <= '0;
            s_cap_v <= 1'b0;
            state <= SP_SLOT;
        end else begin
            case (state)
                SP_IDLE: ;
                SP_SLOT: begin
                    if (!render_now)
                        state <= SP_IDLE;
                    else begin
                        flip_next <= 1'b1;
                        if (s_n < 3'd6)
                            s_n <= s_n + 3'd1;
                        s_cap <= s_n;
                        s_cap_v <= s_n < 3'd6;
                        if (s_cap_v) begin
                            if (s_cap[0])
                                slot_cfg[s_cap[2:1]] <= s_data;
                            else
                                slot_entry[s_cap[2:1]] <= s_data;
                            if (s_cap == 3'd5) begin
                                p <= '0;
                                state <= SP_PLAN;
                            end
                        end
                    end
                end
                SP_PLAN: begin
                    if (!sp_en)
                        next_plane();
                    else begin
                        run4 <= sp_is4;
                        if (sp_is4)
                            m4_start <= 1'b1;
                        else
                            m5_start <= 1'b1;
                        state <= SP_RUN;
                    end
                end
                SP_RUN: begin
                    if (run4 ? m4_done : m5_done)
                        next_plane();
                end
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_sprite;
    always_comb unused_sprite = ^{t[9], slot_entry[0][30:19],
                                      slot_entry[1][30:19],
                                      slot_entry[2][30:19]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
