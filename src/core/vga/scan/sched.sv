/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The fill scheduler, one engine for three planes. At line start it
 * reads the three fill slots, queues the enabled fills in ascending
 * plane order, and runs them through fill one at a time. A plane that
 * does not run never flips its line buffer, so it scans out zeros.
 *
 * A line is 1,600 clocks (timing.sv) and a fill lands two pixels a
 * clock, so three planes fit serially on a 640-wide canvas. A 320-wide
 * canvas pairs its lines, one row of graphics to two lines of timing, so
 * a row there has 3,200 clocks and room to spare.
 *
 * An enabled slot in mode 0 runs no fill. It marks the plane whose
 * pixels come from the terminal engine instead; host/pocket/core/
 * wiring.sv selects on sched_term.
 */

module sched (
    input logic clk,
    input logic rst_n,

    input logic [9:0] v,
    input logic [9:0] h,
    input logic line_start,

    input logic [9:0] cw,
    input logic [9:0] ch,

    /* The row map, which every stage pairs lines by. It lives here
     * because this is the one instance that walks the frame's rows, and
     * mode0.sv, scan/sprite.sv and the top level all read it rather than
     * deriving it again. */
    output logic [9:0] sched_t,
    output logic sched_dbl,
    output logic sched_pair_start,
    output logic sched_pair_end,
    output logic sched_render_now,

    output logic [8:0] sched_p_line,
    output logic [1:0] sched_p_plane,
    input logic [31:0] p_entry,
    input logic [15:0] p_config,

    output logic sched_e_start,
    output logic [2:0] sched_e_mode,
    output logic [15:0] sched_e_attr,
    output logic [15:0] sched_e_config,
    input logic e_done,
    input logic [1:0] e_px_we,

    output logic [1:0] sched_px_we[3],
    output logic [2:0] sched_done,
    output logic [2:0] sched_term
);

    logic [9:0] t /*verilator public_flat_rd*/;
    /* A 320 wide canvas is scanned out with its lines doubled, so a row of
     * graphics spans two lines of timing and the beam keeps step with the
     * 6502. */
    logic dbl;
    always_comb dbl = cw == 10'd320;
    always_comb sched_dbl = dbl;
    logic [9:0] v_next;
    always_comb v_next = v == 10'd524 ? 10'd0 : v + 10'd1;
    /* Lines pair as (0,1), (2,3) ... with (523,524) for row 0, so a row
     * is started on the even line and has the whole pair to finish in, and
     * is handed over on the even line after. Line 524 is the pair's second
     * line, not a start, and 523 is a start, not an end. */
    logic pair_start, pair_end;
    always_comb pair_start = !dbl || (!v[0] && v != 10'd524) || v == 10'd523;
    always_comb pair_end = !dbl || (v[0] && v != 10'd523) || v == 10'd524;
    always_comb sched_pair_start = pair_start;
    always_comb sched_pair_end = pair_end;
    always_comb sched_t = t;

    logic render_now;
    always_comb render_now = t < ch;
    always_comb sched_render_now = render_now;
    logic [8:0] t_row;
    always_comb t_row = t[8:0];
    always_comb sched_p_line = t_row;

    typedef enum logic [1:0] {
        SCH_IDLE, SCH_READ, SCH_RUN
    } state_t;
    state_t state /*verilator public_flat_rd*/;

    /* The slot sweep presents plane 0, 1 and 2 on consecutive clocks and
     * latches each answer a clock behind, because the slot memory is
     * registered, so the queue is decided on the fifth clock. */
    logic [2:0] rd_i;
    always_comb sched_p_plane = rd_i[1:0];

    logic pl_en[3];
    logic pl_term[3];
    logic [2:0] pl_mode[3];
    logic [15:0] pl_attr[3];
    logic [15:0] pl_cfg[3];

    /* The planes still to fill this row run lowest first, so the one
     * running is the lowest pending, and the fill reads its slot straight
     * from here for as long as it runs. */
    logic [2:0] plane_pending /*verilator public_flat_rd*/;
    logic [1:0] cur;
    always_comb begin
        cur = plane_pending[0] ? 2'd0 : plane_pending[1] ? 2'd1 : 2'd2;
        sched_e_mode = pl_mode[cur];
        sched_e_attr = pl_attr[cur];
        sched_e_config = pl_cfg[cur];
    end

    /* The marker compose sees is decided during this line's slot sweep
     * and taken at the next line_start, so it changes on the same edge
     * the line buffers flip banks. */
    logic [2:0] term_q, term_dec;
    logic term_armed;
    always_comb term_dec = {pl_term[2], pl_term[1], pl_term[0]};
    always_comb sched_term = term_q;

    always_comb begin
        sched_done = '0;
        if (state == SCH_RUN && e_done)
            sched_done[cur] = 1'b1;
        for (int i = 0; i < 3; i++)
            sched_px_we[i] = 2'b00;
        if (state == SCH_RUN)
            sched_px_we[cur] = e_px_we;
    end

`ifdef VERILATOR
    /* The underrun check below arms at the first line 524 after reset,
     * because timing.sv takes no reset: a machine reset lands somewhere
     * inside a frame, and the fills it interrupts legitimately do not
     * finish. */
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
        end
        plane_pending = '0;
        term_q = '0;
        term_armed = 1'b0;
        sched_e_start = 1'b0;
    end
    always_ff @(posedge clk) begin
        sched_e_start <= 1'b0;
`ifdef VERILATOR
        if (settled && h == 10'd799 && pair_end && state != SCH_IDLE)
            $fatal(1, "fill underrun");
`endif
        if (line_start) begin
            t <= dbl ? (v >= 10'd523 ? 10'd0 : 10'((v >> 1) + 10'd1))
                     : v_next;
            if (pair_start) begin
                rd_i <= '0;
                plane_pending <= '0;
                if (term_armed)
                    term_q <= term_dec;
                term_armed <= 1'b0;
                state <= SCH_READ;
            end
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
                            plane_pending <= {pl_en[2], pl_en[1], pl_en[0]};
                            term_armed <= 1'b1;
                            if (pl_en[0] || pl_en[1] || pl_en[2]) begin
                                sched_e_start <= 1'b1;
                                state <= SCH_RUN;
                            end else
                                state <= SCH_IDLE;
                        end
                    end
                end
                SCH_RUN: begin
                    if (e_done) begin
                        plane_pending[cur] <= 1'b0;
                        if ((plane_pending & ~(3'd1 << cur)) != 3'd0)
                            sched_e_start <= 1'b1;
                        else
                            state <= SCH_IDLE;
                    end
                end
                default: state <= SCH_IDLE;
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_sched;
    always_comb unused_sched = ^{p_entry[30:19]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
