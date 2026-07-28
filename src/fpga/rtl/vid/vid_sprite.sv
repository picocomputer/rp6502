/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The sprite stage: vga.c's foreground walk, run once per rendered line
 * after every plane's fill has finished. Sprites do not own a buffer —
 * they paint into the foreground, the most recently filled plane at or
 * below their own, and only when no plane below has filled do they zero
 * their own bank and claim it as a filled layer. Plane order is the
 * paint order, so the walk is sequential where the fills raced in
 * parallel; everything still lands before the beam's h==799 read of the
 * next line's first pixel.
 */

module vid_sprite (
    input logic clk,
    input logic rst_n,

    input logic [9:0] v,
    input logic [9:0] h,
    input logic line_start,

    input logic console,
    input logic x_shift,
    input logic y_shift,
    input logic [9:0] y_offset,

    /* The sprite slots out of the prog table, one word per clock. */
    output logic [12:0] vid_sprite_s_idx,
    input logic [31:0] s_data,

    /* The fill engines' line outcome. */
    input logic [2:0] busy,
    input logic [2:0] rnew,
    input logic [2:0] rfilled,

    /* Writes into the foreground plane's bank, and the claim that turns
     * a zeroed bank into a filled layer. */
    output logic [1:0] vid_sprite_plane,
    output logic vid_sprite_we,
    output logic [9:0] vid_sprite_addr,
    output logic [15:0] vid_sprite_data,
    output logic vid_sprite_force,

    /* XRAM port A, one rotor slot. */
    output logic vid_sprite_a_req,
    output logic [13:0] vid_sprite_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata
);

    /* Target line, the fills' own derivation. */
    logic [9:0] t;
    logic [9:0] t_cv;
    logic render_now;
    always_comb begin
        t_cv = t - y_offset;
        render_now = !console && t >= y_offset && t < 10'd480
            && !(y_shift && t_cv[0]);
    end
    logic [8:0] t_row;
    always_comb t_row = y_shift ? t_cv[9:1] : t_cv[8:0];
    logic [9:0] cw;
    always_comb cw = x_shift ? 10'd320 : 10'd640;

    typedef enum logic [2:0] {
        SP_IDLE, SP_SLOT, SP_WAIT, SP_PLAN, SP_CLEAR, SP_RUN
    } state_t;
    state_t state;

    /* The three sprite slots, read at line start: present an index each
     * clock, the word answers the next. */
    logic [31:0] slot_entry[3];
    logic [31:0] slot_cfg[3];
    logic [2:0] s_n, s_cap;
    logic s_cap_v;
    always_comb vid_sprite_s_idx = {t_row, s_n[2:1], 1'b1, s_n[0]};

    logic [1:0] p;       /* the plane being walked */
    logic fg_v;
    logic [1:0] fg;
    logic [9:0] clr;

    /* The two sprite engines; the slot's mode bits pick one. */
    logic m5_start;
    logic m5_a_req;
    logic [13:0] m5_a_addr;
    logic m5_px_we;
    logic [9:0] m5_px_addr;
    logic [15:0] m5_px_data;
    logic m5_done;
    vid_mode5 vid_mode5 (
        .clk(clk),
        .rst_n(rst_n),
        .start(m5_start),
        .abort_i(line_start),
        .attr(slot_entry[p][15:0]),
        .cfg(slot_cfg[p][15:0]),
        .length(slot_cfg[p][31:16]),
        .t_row(t_row),
        .cw(cw),
        .vid_mode5_a_req(m5_a_req),
        .vid_mode5_a_addr(m5_a_addr),
        .a_gnt(a_gnt),
        .a_rdata(a_rdata),
        .vid_mode5_px_we(m5_px_we),
        .vid_mode5_px_addr(m5_px_addr),
        .vid_mode5_px_data(m5_px_data),
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
        .rst_n(rst_n),
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

    always_comb begin
        vid_sprite_a_req = state == SP_RUN && (sp_is4 ? m4_a_req : m5_a_req);
        vid_sprite_a_addr = sp_is4 ? m4_a_addr : m5_a_addr;
    end

    logic sp_en;
    always_comb sp_en = slot_entry[p][31]
        && (slot_entry[p][18:16] == 3'd5 || sp_is4);

    always_comb begin
        vid_sprite_plane = state == SP_CLEAR ? p : fg;
        vid_sprite_we = 1'b0;
        vid_sprite_addr = clr;
        vid_sprite_data = 16'h0000;
        if (state == SP_CLEAR)
            vid_sprite_we = 1'b1;
        else if (state == SP_RUN) begin
            vid_sprite_we = sp_is4 ? m4_px_we : m5_px_we;
            vid_sprite_addr = sp_is4 ? m4_px_addr : m5_px_addr;
            vid_sprite_data = sp_is4 ? m4_px_data : m5_px_data;
        end
    end

    task automatic next_plane();
        if (p == 2'd2)
            state <= SP_IDLE;
        else begin
            p <= p + 2'd1;
            state <= SP_PLAN;
        end
    endtask

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= SP_IDLE;
            t <= '0;
            slot_entry[0] <= '0;
            slot_entry[1] <= '0;
            slot_entry[2] <= '0;
            slot_cfg[0] <= '0;
            slot_cfg[1] <= '0;
            slot_cfg[2] <= '0;
            s_n <= '0;
            s_cap <= '0;
            s_cap_v <= 1'b0;
            p <= '0;
            fg_v <= 1'b0;
            fg <= '0;
            clr <= '0;
            m4_start <= 1'b0;
            m5_start <= 1'b0;
            vid_sprite_force <= 1'b0;
        end else begin
            m4_start <= 1'b0;
            m5_start <= 1'b0;
            vid_sprite_force <= 1'b0;
            /* The same deadline the fills answer to. */
            if (h == 10'd799 && state != SP_IDLE)
                $fatal(1, "vid_sprite underrun st=%0d p=%0d fg=%0d e=%08X",
                       state, p, fg, slot_entry[p]);
            if (line_start) begin
                t <= v == 10'd524 ? 10'd0 : v + 10'd1;
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
                                    fg_v <= 1'b0;
                                    state <= SP_WAIT;
                                end
                            end
                        end
                    end
                    SP_WAIT: begin
                        if (busy == 3'b000)
                            state <= SP_PLAN;
                    end
                    SP_PLAN: begin
                        if (rnew[p] && rfilled[p]) begin
                            fg_v <= 1'b1;
                            fg <= p;
                        end
                        if (!sp_en)
                            next_plane();
                        else if (!(rnew[p] && rfilled[p]) && !fg_v) begin
                            clr <= '0;
                            state <= SP_CLEAR;
                        end else begin
                            if (sp_is4)
                                m4_start <= 1'b1;
                            else
                                m5_start <= 1'b1;
                            state <= SP_RUN;
                        end
                    end
                    SP_CLEAR: begin
                        clr <= clr + 10'd1;
                        if (clr == cw - 10'd1) begin
                            vid_sprite_force <= 1'b1;
                            fg_v <= 1'b1;
                            fg <= p;
                            if (sp_is4)
                                m4_start <= 1'b1;
                            else
                                m5_start <= 1'b1;
                            state <= SP_RUN;
                        end
                    end
                    SP_RUN: begin
                        if (sp_is4 ? m4_done : m5_done)
                            next_plane();
                    end
                    default: state <= SP_IDLE;
                endcase
            end
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_sprite;
    always_comb unused_vid_sprite = ^{t_cv, slot_entry[0][30:19],
                                      slot_entry[1][30:19],
                                      slot_entry[2][30:19]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
