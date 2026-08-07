/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One plane's line buffer: the ping-pong pair the shared fill engine
 * writes through vid_sched's routing, and the beam reads one line
 * behind. The plane keeps only what is truly its own — the buffer, its
 * banks, and the filled flag; the engine that fills it is vid_fill,
 * one for all three planes.
 *
 * On line-doubled canvases a row renders once, on the raster line where
 * it first appears, and the buffer holds through the repeat — never a
 * re-render the oracle could not have seen.
 */

module vid_mode (
    input logic clk,

    input logic [9:0] h,
    input logic px_last,
    input logic line_start,
    input logic [9:0] cw,

    input logic px_we,
    input logic [9:0] px_addr,
    input logic [15:0] px_data,

    /* done says the line resolved; filled says the buffer holds it. A
     * done without filled leaves the buffer unwritten — the compose
     * skips an unfilled plane, and nothing else reads it. */
    input logic done_i,
    input logic filled_i,

    output logic [15:0] vid_mode_pix,
    output logic vid_mode_filled
);

    /* The bank rides inside the address and the output register carries
     * only the buffer: either one broken keeps the line out of block
     * memory. */
    (* ramstyle = "no_rw_check" *)
    logic [15:0] linebuf[2048];
    logic wr_bank;
    logic filled_q[2] /*verilator public_flat_rd*/;
    logic flip_next;

    /* The beam reads one ahead of itself, on each pixel's last tick, and
     * the bank flip lands on h==0's first tick — so only the pixel-0
     * read at the end of h==799 sees the fresh line under its write-side
     * label. A repeat line never flips and reads the held bank. */
    logic [9:0] rd_next;
    always_comb rd_next = h + 10'd1;
    logic [10:0] lb_rd;
    always_comb lb_rd = h == 10'd799
        ? {flip_next ? wr_bank : !wr_bank, 10'd0}
        : {!wr_bank, rd_next};

    logic [15:0] lb_q;
    logic lb_blank;
    always_ff @(posedge clk) begin
        if (px_last) begin
            lb_q <= linebuf[lb_rd];
            lb_blank <= !(h == 10'd799 || h < cw - 10'd1);
        end
    end
    always_comb vid_mode_pix = lb_blank ? 16'h0000 : lb_q;
    always_comb vid_mode_filled = filled_q[!wr_bank];

    always_ff @(posedge clk) begin
        if (px_we)
            linebuf[{wr_bank, px_addr}] <= px_data;
    end

    initial begin
        wr_bank = 1'b0;
        filled_q[0] = 1'b0;
        filled_q[1] = 1'b0;
        flip_next = 1'b0;
    end
    /* The next line's pixel 0 is read during h==799, so the flip must
     * land before it or that pixel comes up stale. */
    always_ff @(posedge clk) begin
        if (line_start) begin
            if (flip_next)
                wr_bank <= !wr_bank;
            flip_next <= 1'b0;
        end else if (done_i) begin
            filled_q[wr_bank] <= filled_i;
            flip_next <= 1'b1;
        end
    end

endmodule
