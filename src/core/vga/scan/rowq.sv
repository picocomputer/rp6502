/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The row word queue both sprite engines share. A sprite row's words
 * stream in ahead of its pixels, a word asked for whenever the queue and
 * the two clocks in flight can take it, up to the row's last, so the head
 * of the queue is always the word the pixel is in and a word boundary is
 * a pop. The engines differ only in where a row starts and ends and in
 * when they pop.
 */

module rowq (
    input logic clk,

    /* The engine is streaming a row. Out of it the queue forgets what it
     * read ahead, and words still landing for the row just left are not
     * taken for the next. */
    input logic run,
    input logic [13:0] first,
    input logic [13:0] last,
    input logic want,
    input logic pop,
    /* A pop this clock that the request may count on as room. */
    input logic pop_room,

    input logic gnt,
    input logic [31:0] a_rdata,

    output logic rowq_req,
    output logic [13:0] rowq_addr,
    output logic [31:0] rowq_q0,
    output logic [31:0] rowq_q1,
    output logic [2:0] rowq_n
);

    logic [31:0] q[4];
    logic [2:0] qn;
    logic [13:0] fp;    /* the next word to ask for, once one has been */
    logic fp_v;
    /* XRAM answers two clocks after a grant. */
    logic flight, land;

    always_comb begin
        rowq_addr = fp_v ? fp : first;
        rowq_req = run && want && (!fp_v || fp <= last)
            && qn + {2'd0, land} + {2'd0, flight} - {2'd0, pop_room} < 3'd4;
        rowq_q0 = q[0];
        rowq_q1 = q[1];
        rowq_n = qn;
    end

    initial begin
        for (int j = 0; j < 4; j++)
            q[j] = '0;
        qn = '0;
        fp = '0;
        fp_v = 1'b0;
        flight = 1'b0;
        land = 1'b0;
    end
    always_ff @(posedge clk) begin
        flight <= rowq_req && gnt;
        land <= flight;
        if (!run) begin
            qn <= '0;
            fp_v <= 1'b0;
        end else begin
            if (rowq_req && gnt) begin
                fp <= rowq_addr + 14'd1;
                fp_v <= 1'b1;
            end
            if (pop) begin
                q[0] <= q[1];
                q[1] <= q[2];
                q[2] <= q[3];
            end
            if (land)
                q[2'(pop ? qn - 3'd1 : qn)] <= a_rdata;
            qn <= qn + {2'd0, land} - {2'd0, pop};
        end
    end

endmodule
