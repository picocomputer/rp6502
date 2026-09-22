/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The descriptor list queue both sprite engines share. The list streams
 * in ahead of the sprites, a word asked for whenever the row leaves the
 * slot free and the queue's eight words and the two clocks in flight can
 * take one, up to the list's last word. It also counts the descriptors
 * left, so an engine knows the one it pops is the last. A descriptor is read out of the queue's first
 * words and popped whole, so a list on a halfword boundary differs only
 * in where the window starts, and a descriptor an odd number of halfwords
 * long moves the next one to the other half.
 */

module listq (
    input logic clk,

    input logic start,
    input logic [15:0] cfg,
    input logic [13:0] last,
    input logic [13:0] length,
    input logic active,

    input logic [3:0] size,     /* a descriptor's halfwords */
    input logic pop,

    input logic gnt,
    input logic [31:0] a_rdata,

    output logic listq_req,
    output logic [13:0] listq_addr,
    output logic [159:0] listq_dsc,
    output logic listq_v,
    output logic listq_end
);

    logic [31:0] dq[8];
    logic [3:0] dqn;
    logic [13:0] dfp, dend;
    logic half;             /* the next descriptor starts mid-word */
    logic [13:0] n_left;
    /* XRAM answers two clocks after a grant. */
    logic flight, land;

    /* The halfwords from the window's start to the descriptor's end. */
    logic [4:0] span;
    logic [3:0] pop_n;
    logic [175:0] dwin;
    always_comb begin
        span = {4'd0, half} + {1'b0, size};
        pop_n = span[4:1];
        dwin = {dq[5][15:0], dq[4], dq[3], dq[2], dq[1], dq[0]};
        listq_dsc = half ? dwin[175:16] : dwin[159:0];
        listq_v = {1'b0, dqn} >= 5'(span + 5'd1) >> 1;
        listq_addr = dfp;
        listq_req = active && dfp <= dend
            && dqn + {3'd0, land} + {3'd0, flight} < 4'd8;
        listq_end = n_left == 14'd0;
    end

    initial begin
        for (int j = 0; j < 8; j++)
            dq[j] = '0;
        dqn = '0;
        dfp = '0;
        dend = '0;
        half = 1'b0;
        n_left = '0;
        flight = 1'b0;
        land = 1'b0;
    end
    always_ff @(posedge clk) begin
        flight <= gnt;
        land <= flight;
        if (start) begin
            dqn <= '0;
            dfp <= cfg[15:2];
            dend <= last;
            half <= cfg[1];
            n_left <= length;
        end else begin
            if (gnt)
                dfp <= dfp + 14'd1;
            if (pop) begin
                half <= span[0];
                n_left <= n_left - 14'd1;
                case (pop_n)
                    4'd2: for (int j = 0; j < 6; j++) dq[j] <= dq[j + 2];
                    4'd3: for (int j = 0; j < 5; j++) dq[j] <= dq[j + 3];
                    default: for (int j = 0; j < 3; j++) dq[j] <= dq[j + 5];
                endcase
            end
            if (land && active)
                dq[3'(pop ? dqn - pop_n : dqn)] <= a_rdata;
            dqn <= dqn + {3'd0, land && active} - (pop ? pop_n : 4'd0);
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_listq;
    always_comb unused_listq = ^{cfg[0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
