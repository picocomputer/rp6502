/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The fill scheduler: one engine for three planes. At line start it
 * reads the three fill slots, queues the enabled fills ascending, and
 * runs them through vid_fill one at a time; a plane that does not run
 * simply never flips, and its scan bank is the eraser's zeros. A
 * 640-wide canvas has serial fill rate for two planes, a 320-wide for
 * three; the lowest-numbered fills win. A mode-0 slot runs no fill —
 * it marks the plane whose stream is the terminal engine's, the same
 * way the sprite stage is its own engine beside this one. The firmware
 * registers mode 0 exclusively, so the marker is a plain mask with no
 * defense against several.
 */

module vid_sched (
    input logic clk,
    input logic rst_n,

    input logic [9:0] v,
    input logic [9:0] h,
    input logic line_start,

    input logic [9:0] cw,
    input logic [9:0] ch,

    output logic [8:0] vid_sched_p_line,
    output logic [1:0] vid_sched_p_plane,
    input logic [31:0] p_entry,
    input logic [15:0] p_config,

    output logic vid_sched_e_start,
    output logic [2:0] vid_sched_e_mode,
    output logic [15:0] vid_sched_e_attr,
    output logic [15:0] vid_sched_e_config,
    input logic e_done,
    input logic e_px_we,

    output logic [2:0] vid_sched_px_we,
    output logic [2:0] vid_sched_done,
    output logic [2:0] vid_sched_term
);

    logic [9:0] t /*verilator public_flat_rd*/;
    logic render_now;
    always_comb render_now = t < ch;
    logic [8:0] t_row;
    always_comb t_row = t[8:0];
    always_comb vid_sched_p_line = t_row;

    typedef enum logic [1:0] {
        SCH_IDLE, SCH_READ, SCH_RUN
    } state_t;
    state_t state /*verilator public_flat_rd*/;

    /* The slot sweep: present plane 0,1,2 on consecutive clocks, latch
     * each registered answer a clock behind, decide on the fifth. */
    logic [2:0] rd_i;
    always_comb vid_sched_p_plane = rd_i[1:0];

    logic pl_en[3];
    logic pl_term[3];
    logic [2:0] pl_mode[3];
    logic [15:0] pl_attr[3];
    logic [15:0] pl_cfg[3];

    logic [1:0] dec_q[3];
    logic [1:0] dec_n;
    logic [2:0] dec_run;
    always_comb begin
        dec_q[0] = 2'd0;
        dec_q[1] = 2'd0;
        dec_q[2] = 2'd0;
        dec_n = 2'd0;
        for (int i = 0; i < 3; i++)
            if (pl_en[i]) begin
                dec_q[dec_n] = 2'(i);
                dec_n = dec_n + 2'd1;
            end
        if (cw == 10'd640 && dec_n == 2'd3)
            dec_n = 2'd2;
        dec_run = '0;
        for (int i = 0; i < 3; i++)
            if (2'(i) < dec_n)
                dec_run[dec_q[i]] = 1'b1;
    end

    logic [1:0] q[3];
    logic [1:0] q_n, q_i;
    logic [1:0] cur;
    always_comb cur = q[q_i];
    logic [2:0] plane_pending /*verilator public_flat_rd*/;

    /* The marker the compose sees updates with the bank flips: decided
     * mid-render-line, taken at the next line_start, held with the
     * banks otherwise. */
    logic [2:0] term_q, term_dec;
    logic term_armed;
    always_comb term_dec = {pl_term[2], pl_term[1], pl_term[0]};
    always_comb vid_sched_term = term_q;

    /* The done a shell sees is the engine's own edge, forwarded without
     * a register: the flip must land on sub_done's clock or the h==799
     * pixel-0 pre-read comes up stale. */
    always_comb begin
        vid_sched_done = '0;
        if (state == SCH_RUN && e_done)
            vid_sched_done[cur] = 1'b1;
        vid_sched_px_we = '0;
        if (state == SCH_RUN && e_px_we)
            vid_sched_px_we[cur] = 1'b1;
    end

`ifdef VERILATOR
    /* Simulation only: VERILATOR is defined by the simulator and by
     * nothing in the synthesis flow, so the fabric never sees the port.
     * A machine reset costs the beam a frame, because the beam does not
     * take one; this tells the stop below to expect it. */
    logic settled;
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n)
            settled <= 1'b0;
        else if (line_start && v == 10'd524)
            settled <= 1'b1;
`endif

    initial begin
        state = SCH_IDLE;
        t = '0;
        rd_i = '0;
        for (int i = 0; i < 3; i++) begin
            pl_en[i] = 1'b0;
            pl_term[i] = 1'b0;
            pl_mode[i] = '0;
            pl_attr[i] = '0;
            pl_cfg[i] = '0;
            q[i] = '0;
        end
        q_n = '0;
        q_i = '0;
        plane_pending = '0;
        term_q = '0;
        term_armed = 1'b0;
        vid_sched_e_start = 1'b0;
        vid_sched_e_mode = '0;
        vid_sched_e_attr = '0;
        vid_sched_e_config = '0;
    end
    always_ff @(posedge clk) begin
        vid_sched_e_start <= 1'b0;
`ifdef VERILATOR
        /* The stop waits one frame, which is what a reset costs a beam
         * that does not take one — a frame of the black screen the
         * machine boots to. */
        if (settled && h == 10'd799 && state != SCH_IDLE)
            $fatal(1, "vid_fill underrun");
`endif
        if (line_start) begin
            t <= v == 10'd524 ? 10'd0 : v + 10'd1;
            rd_i <= '0;
            plane_pending <= '0;
            if (term_armed)
                term_q <= term_dec;
            term_armed <= 1'b0;
            state <= SCH_READ;
        end else begin
            case (state)
                SCH_IDLE: ;
                SCH_READ: begin
                    if (rd_i == 3'd0 && !render_now)
                        state <= SCH_IDLE;
                    else begin
                        rd_i <= rd_i + 3'd1;
                        if (rd_i >= 3'd1 && rd_i <= 3'd3) begin
                            pl_en[2'(rd_i - 3'd1)] <= p_entry[31]
                                && p_entry[18:16] != 3'd0
                                && p_entry[18:16] <= 3'd3;
                            pl_term[2'(rd_i - 3'd1)] <= p_entry[31]
                                && p_entry[18:16] == 3'd0;
                            pl_mode[2'(rd_i - 3'd1)] <= p_entry[18:16];
                            pl_attr[2'(rd_i - 3'd1)] <= p_entry[15:0];
                            pl_cfg[2'(rd_i - 3'd1)] <= p_config;
                        end
                        if (rd_i == 3'd4) begin
                            for (int i = 0; i < 3; i++)
                                q[i] <= dec_q[i];
                            q_n <= dec_n;
                            q_i <= 2'd0;
                            plane_pending <= dec_run;
                            term_armed <= 1'b1;
                            if (dec_n != 2'd0) begin
                                vid_sched_e_start <= 1'b1;
                                vid_sched_e_mode <= pl_mode[dec_q[0]];
                                vid_sched_e_attr <= pl_attr[dec_q[0]];
                                vid_sched_e_config <= pl_cfg[dec_q[0]];
                                state <= SCH_RUN;
                            end else
                                state <= SCH_IDLE;
                        end
                    end
                end
                SCH_RUN: begin
                    if (e_done) begin
                        plane_pending[cur] <= 1'b0;
                        if (q_i + 2'd1 < q_n) begin
                            q_i <= q_i + 2'd1;
                            vid_sched_e_start <= 1'b1;
                            vid_sched_e_mode <= pl_mode[q[q_i + 2'd1]];
                            vid_sched_e_attr <= pl_attr[q[q_i + 2'd1]];
                            vid_sched_e_config <= pl_cfg[q[q_i + 2'd1]];
                        end else
                            state <= SCH_IDLE;
                    end
                end
                default: state <= SCH_IDLE;
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_sched;
    always_comb unused_vid_sched = ^{p_entry[30:19]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
