/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module vid_sprite (
    input logic clk,

    input logic [9:0] v,
    input logic [9:0] h,
    input logic px_last,
    input logic line_start,

    input logic [9:0] cw,
    input logic [9:0] ch,

    output logic [12:0] vid_sprite_s_idx,
    input logic [31:0] s_data,

    output logic [16:0] vid_sprite_pix[3],

    output logic [15:0] vid_sprite_overrun ,

    output logic vid_sprite_a_req,
    output logic [13:0] vid_sprite_a_addr,
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

    logic [31:0] slot_entry[3];
    logic [31:0] slot_cfg[3];
    logic [2:0] s_n, s_cap;
    logic s_cap_v;
    always_comb vid_sprite_s_idx = {t_row, s_n[2:1], 1'b1, s_n[0]};

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
    vid_palcache vid_palcache (
        .clk(clk),
        .lookup(pal_lookup),
        .xram(pal_xram),
        .one_bpp(pal_one_bpp),
        .base(pal_base),
        .idx_a(pal_idx),
        .idx_b(8'd0),
        .need_b(1'b0),
        .vid_palcache_qa(pal_q),
        .vid_palcache_qb(),
        .vid_palcache_hit(pal_hit),
        .vid_palcache_req(pc_req),
        .vid_palcache_addr(pc_addr),
        .fill_gnt(pc_gnt),
        .fill_rdy(pc_rdy),
        .a_rdata(a_rdata),
        .flush(line_start)
    );

    vid_mode5 vid_mode5 (
        .clk(clk),
        .start(m5_start),
        .abort_i(line_start),
        .attr(slot_entry[p][15:0]),
        .cfg(slot_cfg[p][15:0]),
        .length(slot_cfg[p][31:16]),
        .t_row(t_row),
        .cw(cw),
        .vid_mode5_a_req(m5_a_req),
        .vid_mode5_a_addr(m5_a_addr),
        .a_gnt(a_gnt && !pc_req),
        .a_rdata(a_rdata),
        .vid_mode5_px_we(m5_px_we),
        .vid_mode5_px_addr(m5_px_addr),
        .vid_mode5_px_data(m5_px_data),
        .vid_mode5_pal_lookup(pal_lookup),
        .vid_mode5_pal_xram(pal_xram),
        .vid_mode5_pal_one_bpp(pal_one_bpp),
        .vid_mode5_pal_base(pal_base),
        .vid_mode5_pal_idx(pal_idx),
        .pal_hit(pal_hit),
        .pal_q(pal_q),
        .vid_mode5_done(m5_done)
    );
    logic m4_start;
    logic m4_a_req;
    logic [13:0] m4_a_addr;
    logic m4_px_we;
    logic [9:0] m4_px_addr;
    logic [15:0] m4_px_data;
    logic m4_done;
    vid_mode4 vid_mode4 (
        .clk(clk),
        .start(m4_start),
        .abort_i(line_start),
        .attr(slot_entry[p][15:0]),
        .cfg(slot_cfg[p][15:0]),
        .length(slot_cfg[p][31:16]),
        .t_row(t_row),
        .cw(cw),
        .vid_mode4_a_req(m4_a_req),
        .vid_mode4_a_addr(m4_a_addr),
        .a_gnt(a_gnt),
        .a_rdata(a_rdata),
        .vid_mode4_px_we(m4_px_we),
        .vid_mode4_px_addr(m4_px_addr),
        .vid_mode4_px_data(m4_px_data),
        .vid_mode4_done(m4_done)
    );

    logic sp_is4;
    always_comb sp_is4 = slot_entry[p][18:16] == 3'd4;

    logic run4;

    always_comb begin
        vid_sprite_a_req = state == SP_RUN
            && (run4 ? m4_a_req : (pc_req || m5_a_req));
        vid_sprite_a_addr = run4 ? m4_a_addr
            : pc_req ? pc_addr : m5_a_addr;
        pc_gnt = a_gnt && pc_req && state == SP_RUN && !run4;
    end

    logic sp_en;
    always_comb sp_en = slot_entry[p][31]
        && (slot_entry[p][18:16] == 3'd5 || sp_is4);

    logic wr_bank;
    logic flip_next;

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
            vid_sbuf vid_sbuf (
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
                .vid_sbuf_pix(vid_sprite_pix[gi])
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
        vid_sprite_overrun = '0;
    end
    always_ff @(posedge clk) begin
        m4_start <= 1'b0;
        m5_start <= 1'b0;
        if (h == 10'd799 && state != SP_IDLE) begin
            vid_sprite_overrun <= vid_sprite_overrun
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

    logic unused_vid_sprite;
    always_comb unused_vid_sprite = ^{t[9], slot_entry[0][30:19],
                                      slot_entry[1][30:19],
                                      slot_entry[2][30:19]};

endmodule
