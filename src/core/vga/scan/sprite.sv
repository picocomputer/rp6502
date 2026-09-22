/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The sprite stage: three slots, one per plane, each with a ping-pong
 * line buffer of its own (sbuf). Nothing here waits on a fill and nothing
 * clears a buffer, because the scan side erases behind the pixel being
 * displayed, so a slot scans out its written pixels over unmarked ones
 * and the engines can start as soon as the slots are decoded.
 * Sprites paint in list order within a slot; across slots, compose
 * stacks the planes.
 */

module sprite (
    input logic clk,

    /* The beam's derived columns, computed once at the top level because
     * the line buffers of every plane share them. */
    input logic [9:0] sc_addr,
    input logic [9:0] rd_addr,
    input logic h_last,

    input logic px_last,
    input logic line_start,

    input logic [9:0] cw,

    /* The row map, from sched.sv: a 320 wide canvas is scanned out with
     * its lines doubled, so a row of graphics spans two lines of timing
     * and the beam keeps step with the 6502. */
    input logic [8:0] t_row,
    input logic render_now,
    input logic pair_start,
    input logic pair_end,

    output logic [12:0] sprite_s_idx,
    input logic [31:0] s_data,

    output logic [16:0] sprite_pix[3],

    /* The render port's second slot is this stage's alone, so a request
     * is taken on the clock it is made and its word lands two clocks on.
     * Each engine's grant is therefore formed here from the stage's state,
     * which keeps one engine's request logic out of the other's. */
    output logic sprite_a_req,
    output logic [13:0] sprite_a_addr,
    input logic [31:0] a_rdata
);

    /* The engines and the palette cache work a row at a time, so they are
     * aborted and flushed where a row starts, not on the second line of a
     * pair, which would drop a pass that legitimately runs into it. */
    logic row_start;
    always_comb row_start = line_start && pair_start;

    typedef enum logic [1:0] {
        SP_IDLE, SP_SLOT, SP_PLAN, SP_RUN
    } state_t;
    state_t state;

    /* The sprite slot of the plane being drawn, read as the plane is
     * planned: an index goes out each clock and the word answers on the
     * next, the entry then the config. */
    logic [31:0] slot_entry;
    logic [31:0] slot_cfg;
    logic s_cfg;
    always_comb sprite_s_idx = {t_row, p, 1'b1, s_cfg};

    logic [1:0] p;

    logic m5_start;
    logic m5_a_req;
    logic [13:0] m5_a_addr;
    logic [1:0] m5_px_we;
    logic [9:0] m5_px_addr;
    logic [31:0] m5_px_data;
    logic m5_done;

    logic pal_lookup, pal_xram, pal_one_bpp, pal_need_b;
    logic [15:0] pal_base;
    logic [7:0] pal_idx, pal_idx_b;
    logic pal_hit;
    logic [15:0] pal_qa, pal_qb;
    logic pc_req;
    logic [13:0] pc_addr;
    /* XRAM answers the sprite stage two clocks after a grant. */
    logic pc_gnt, pc_rdy1, pc_rdy;
    initial begin
        pc_rdy1 = 1'b0;
        pc_rdy = 1'b0;
    end
    always_ff @(posedge clk) begin
        pc_rdy1 <= pc_gnt;
        pc_rdy <= pc_rdy1;
    end
    palcache palcache (
        .clk(clk),
        .lookup(pal_lookup),
        .xram(pal_xram),
        .one_bpp(pal_one_bpp),
        .base(pal_base),
        .idx_a(pal_idx),
        .idx_b(pal_idx_b),
        .need_b(pal_need_b),
        .palcache_qa(pal_qa),
        .palcache_qb(pal_qb),
        .palcache_hit(pal_hit),
        .palcache_req(pc_req),
        .palcache_addr(pc_addr),
        .fill_gnt(pc_gnt),
        .fill_rdy(pc_rdy),
        .a_rdata(a_rdata),
        .flush(row_start)
    );

    mode5 mode5 (
        .clk(clk),
        .start(m5_start),
        .abort_i(row_start),
        .attr(slot_entry[15:0]),
        .t_row(t_row),
        .cw(cw),
        .mode5_a_req(m5_a_req),
        .mode5_a_addr(m5_a_addr),
        .a_gnt(state == SP_RUN && !pc_req),
        .mode5_lq_active(m5_lq_active),
        .mode5_lq_size(m5_lq_size),
        .mode5_lq_pop(m5_lq_pop),
        .mode5_lq_gnt(m5_lq_gnt),
        .lq_req(lq_req),
        .lq_addr(lq_addr),
        .lq_dsc(lq_dsc[79:0]),
        .lq_v(lq_v),
        .lq_end(lq_end),
        .mode5_rq_run(m5_rq_run),
        .mode5_rq_first(m5_rq_first),
        .mode5_rq_last(m5_rq_last),
        .mode5_rq_want(m5_rq_want),
        .mode5_rq_pop(m5_rq_pop),
        .mode5_rq_pop_room(m5_rq_pop_room),
        .rq_req(rq_req),
        .rq_addr(rq_addr),
        .rq_q0(rq_q0),
        .rq_n(rq_n),
        .mode5_px_we(m5_px_we),
        .mode5_px_addr(m5_px_addr),
        .mode5_px_data(m5_px_data),
        .mode5_pal_lookup(pal_lookup),
        .mode5_pal_xram(pal_xram),
        .mode5_pal_one_bpp(pal_one_bpp),
        .mode5_pal_base(pal_base),
        .mode5_pal_idx(pal_idx),
        .mode5_pal_idx_b(pal_idx_b),
        .mode5_pal_need_b(pal_need_b),
        .pal_hit(pal_hit),
        .pal_qa(pal_qa),
        .pal_qb(pal_qb),
        .mode5_done(m5_done)
    );
    logic m4_start;
    logic m4_a_req;
    logic [13:0] m4_a_addr;
    logic [1:0] m4_px_we;
    logic [9:0] m4_px_addr;
    logic [31:0] m4_px_data;
    logic m4_done;
    mode4 mode4 (
        .clk(clk),
        .start(m4_start),
        .abort_i(row_start),
        .attr(slot_entry[15:0]),
        .t_row(t_row),
        .cw(cw),
        .mode4_a_req(m4_a_req),
        .mode4_a_addr(m4_a_addr),
        .a_gnt(state == SP_RUN && run4),
        .a_rdata(a_rdata),
        .mode4_lq_active(m4_lq_active),
        .mode4_lq_size(m4_lq_size),
        .mode4_lq_pop(m4_lq_pop),
        .mode4_lq_gnt(m4_lq_gnt),
        .lq_req(lq_req),
        .lq_addr(lq_addr),
        .lq_dsc(lq_dsc),
        .lq_v(lq_v),
        .lq_end(lq_end),
        .mode4_rq_run(m4_rq_run),
        .mode4_rq_first(m4_rq_first),
        .mode4_rq_last(m4_rq_last),
        .mode4_rq_want(m4_rq_want),
        .mode4_rq_pop(m4_rq_pop),
        .mode4_rq_pop_room(m4_rq_pop_room),
        .rq_req(rq_req),
        .rq_addr(rq_addr),
        .rq_q0(rq_q0),
        .rq_q1(rq_q1),
        .rq_n(rq_n),
        .mode4_px_we(m4_px_we),
        .mode4_px_addr(m4_px_addr),
        .mode4_px_data(m4_px_data),
        .mode4_done(m4_done)
    );

    logic sp_is4;
    always_comb sp_is4 = slot_entry[18:16] == 3'd4;

    /* Latched when the plane is planned, so the engine select that
     * stands in both the pixel port's data path and the XRAM arbitration
     * is a register rather than a compare. */
    logic run4;

    /* One descriptor list queue and one row word queue for the two
     * engines, which never run at once. A list is eight bytes a
     * descriptor, ten for mode 5's custom ones and twenty for mode 4's
     * affine ones, and is sized on the start clock, when run4 and p are
     * this plane's. */
    logic [12:0] n_dsc;
    logic wide;
    logic [16:0] list_end;
    always_comb begin
        n_dsc = slot_cfg[28:16];
        wide = run4 && slot_entry[0];
        list_end = {1'b0, slot_cfg[15:0]}
            + (wide ? {n_dsc, 4'b0} : {1'b0, n_dsc, 3'b0})
            + (run4 ? (wide ? {2'b0, n_dsc, 2'b0} : 17'd0)
                    : (slot_entry[5:3] == 3'd7 ? {3'b0, n_dsc, 1'b0}
                                                  : 17'd0))
            - 17'd1;
    end
    logic m4_lq_active, m4_lq_pop, m4_lq_gnt;
    logic m5_lq_active, m5_lq_pop, m5_lq_gnt;
    logic [3:0] m4_lq_size, m5_lq_size;
    logic lq_req, lq_v, lq_end;
    logic [13:0] lq_addr;
    logic [159:0] lq_dsc;
    listq listq (
        .clk(clk),
        .start(m4_start || m5_start),
        .cfg(slot_cfg[15:0]),
        .last(list_end[15:2]),
        .length(slot_cfg[29:16]),
        .active(run4 ? m4_lq_active : m5_lq_active),
        .size(run4 ? m4_lq_size : m5_lq_size),
        .pop(run4 ? m4_lq_pop : m5_lq_pop),
        .gnt(run4 ? m4_lq_gnt : m5_lq_gnt),
        .a_rdata(a_rdata),
        .listq_req(lq_req),
        .listq_addr(lq_addr),
        .listq_dsc(lq_dsc),
        .listq_v(lq_v),
        .listq_end(lq_end)
    );

    logic m4_rq_run, m4_rq_want, m4_rq_pop, m4_rq_pop_room;
    logic [13:0] m4_rq_first, m4_rq_last;
    logic m5_rq_run, m5_rq_want, m5_rq_pop, m5_rq_pop_room;
    logic [13:0] m5_rq_first, m5_rq_last;
    logic rq_req;
    logic [13:0] rq_addr;
    logic [31:0] rq_q0, rq_q1;
    logic [2:0] rq_n;
    rowq rowq (
        .clk(clk),
        .run(run4 ? m4_rq_run : m5_rq_run),
        .first(run4 ? m4_rq_first : m5_rq_first),
        .last(run4 ? m4_rq_last : m5_rq_last),
        .want(run4 ? m4_rq_want : m5_rq_want),
        .pop(run4 ? m4_rq_pop : m5_rq_pop),
        .pop_room(run4 ? m4_rq_pop_room : m5_rq_pop_room),
        .gnt(state == SP_RUN && (run4 || !pc_req)),
        .a_rdata(a_rdata),
        .rowq_req(rq_req),
        .rowq_addr(rq_addr),
        .rowq_q0(rq_q0),
        .rowq_q1(rq_q1),
        .rowq_n(rq_n)
    );

    /* The cache's fill preempts mode 5's own requests. A palette lookup
     * only happens while the index word is in hand, which is also the only
     * time mode 5 requests a prefetch of the next one, so the slot goes to
     * the cache while pc_req is set and mode 5 is not granted. */
    always_comb begin
        sprite_a_req = state == SP_RUN
            && (run4 ? m4_a_req : (pc_req || m5_a_req));
        sprite_a_addr = run4 ? m4_a_addr
            : pc_req ? pc_addr : m5_a_addr;
        pc_gnt = pc_req && state == SP_RUN && !run4;
    end


    logic wr_bank;
    logic flip_next;

    /* The bank flip lands on h==0's first clock, so only the pixel-0
     * read at the end of h==799 has to take the bank the flip is about
     * to turn into the scan bank. The erase runs the width of the scan
     * rather than stopping at cw, so a switch to a narrower canvas
     * cannot strand pixels. */
    logic [10:0] sb_rd;
    always_comb sb_rd = h_last && pair_end
        ? {flip_next ? wr_bank : !wr_bank, 10'd0}
        : {!wr_bank, rd_addr};
    logic sb_we;
    always_comb sb_we = !px_last;

    logic [1:0] eng_we;
    logic [9:0] eng_addr;
    logic [31:0] eng_px;
    always_comb begin
        eng_we = run4 ? m4_px_we : m5_px_we;
        if (state != SP_RUN)
            eng_we = 2'b00;
        eng_addr = run4 ? m4_px_addr : m5_px_addr;
        eng_px = run4 ? m4_px_data : m5_px_data;
    end

    genvar gi;
    generate
        for (gi = 0; gi < 3; gi++) begin : gen_sbuf
            sbuf sbuf (
                .clk(clk),
                .wr_bank(wr_bank),
                .a_we(eng_we & {2{p == 2'(gi)}}),
                .a_addr(eng_addr),
                .a_data(eng_px),
                .sc_we(sb_we),
                .sc_addr(sc_addr),
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
            state <= SP_SLOT;
        end
    endtask

    initial begin
        state = SP_IDLE;
        slot_entry = '0;
        slot_cfg = '0;
        s_cfg = 1'b0;
        p = '0;
        m4_start = 1'b0;
        m5_start = 1'b0;
        run4 = 1'b0;
        wr_bank = 1'b0;
        flip_next = 1'b0;
    end
    always_ff @(posedge clk) begin
        m4_start <= 1'b0;
        m5_start <= 1'b0;
        if (h_last && pair_end && state != SP_IDLE) begin
            /* The row did not finish in its line; drop it, and the
             * engines abort at the next row start. What it painted
             * before the deadline is what shows. */
            state <= SP_IDLE;
        end else if (row_start) begin
            if (flip_next)
                wr_bank <= !wr_bank;
            flip_next <= 1'b0;
            p <= '0;
            s_cfg <= 1'b0;
            state <= SP_SLOT;
        end else begin
            case (state)
                SP_IDLE: ;
                SP_SLOT: begin
                    if (!render_now)
                        state <= SP_IDLE;
                    else begin
                        flip_next <= 1'b1;
                        s_cfg <= !s_cfg;
                        /* A plane with no sprites is passed over on the
                         * clock its entry answers. */
                        if (s_cfg) begin
                            slot_entry <= s_data;
                            if (s_data[31] && (s_data[18:16] == 3'd4
                                               || s_data[18:16] == 3'd5))
                                state <= SP_PLAN;
                            else
                                next_plane();
                        end
                    end
                end
                /* The config answers on this clock, so the empty list is
                 * judged from it as it is latched. */
                SP_PLAN: begin
                    slot_cfg <= s_data;
                    if (s_data[31:16] == 16'd0)
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
    always_comb unused_sprite = ^{slot_entry[31:19], slot_cfg[31:30],
                                      list_end[16], list_end[1:0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
