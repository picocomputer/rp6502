/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The row mapper every fill mode shares, so that where a line falls in a
 * plane exists once. A mode names its window in pixels and the bytes in
 * one row of its data. This folds the line into the window with true
 * wraparound, makes the oracle's rejects, finds where the line's row of
 * data starts, and walks the line across the window as left padding, the
 * window's run, and right padding. The mode says only which row of its
 * data the line is in, what fills a segment of the run, and how far one
 * may reach.
 */

module rowmap (
    input logic clk,

    input logic start,
    input logic abort_i,
    input logic [95:0] cfgw,
    input logic [8:0] t_row,
    input logic [9:0] cw,

    /* The window in pixels is the config's width and height times these,
     * in int16 like the oracle and taken at start; and the bytes in one
     * row of the plane's data. */
    input logic [4:0] win_wf,
    input logic [4:0] win_hf,
    input logic [19:0] sizeof_row,

    /* The fold ends on the clock rowmap_settle is high, with the row and
     * column in the window unless rowmap_reject. */
    output logic rowmap_settle,
    output logic rowmap_reject,
    output logic signed [16:0] rowmap_row,
    output logic signed [16:0] rowmap_col,
    output logic rowmap_blank,
    output logic rowmap_overrun,

    /* The mode's address clock places its row of data and starts the
     * walk, which runs while the mode is emitting segments. A segment of
     * the window's run waits for the mode to have its content and reaches
     * at most run_max pixels. */
    input logic addr,
    input logic [14:0] data_row,
    output logic [16:0] rowmap_row_base,
    input logic seg_on,
    input logic run_ready,
    input logic [9:0] run_max,
    input logic seg_take,
    output logic rowmap_seg_valid,
    output logic [9:0] rowmap_seg_px,
    output logic rowmap_run,
    output logic rowmap_right,
    output logic rowmap_wrap,
    output logic rowmap_end
);

    logic cf_x_wrap, cf_y_wrap;
    logic signed [15:0] cf_x_pos, cf_y_pos, cf_width;
    logic [14:0] cf_height;
    logic [15:0] cf_data;
    always_comb begin
        cf_x_wrap = cfgw[7:0] != 8'h00;
        cf_y_wrap = cfgw[15:8] != 8'h00;
        cf_x_pos = cfgw[31:16];
        cf_y_pos = cfgw[47:32];
        cf_width = cfgw[63:48];
        cf_height = cfgw[78:64];
        cf_data = cfgw[95:80];
    end

    /* int16 like the oracle: ±32768 wraps before the fold sees it. */
    logic [15:0] row16, col16;
    always_comb row16 = {7'd0, t_row} - 16'(cf_y_pos);
    always_comb col16 = 16'd0 - 16'(cf_x_pos);

    logic fold;
    logic signed [16:0] row, col, w_s, h_s;
    logic blank;
    logic [9:0] px_rem;
    logic [16:0] row_base;

    /* The data overruns XRAM; the oracle's reject. The limit is at most
     * $10000, so any bit above 16 overruns whatever it is and only the
     * low bits reach the comparator. */
    logic [34:0] data_bytes;
    always_comb data_bytes = 35'(cf_height) * 35'(sizeof_row);
    always_comb rowmap_overrun = |data_bytes[34:17]
        || data_bytes[16:0] > 17'(17'h10000 - {1'b0, cf_data});

    /* Iterative wraparound; sane configs settle in a step or two, and the
     * beam's deadline bounds the pathological ones. The oracle rejects on
     * the int16 height, not the count of rows it is made of. */
    logic rejected, y_lo, y_hi, x_lo, x_hi;
    logic signed [16:0] row_up;   /* the fold's step down, whose sign says
                                   * the row is inside */
    always_comb begin
        row_up = row - h_s;
        rejected = cf_width < 16'sd1 || h_s < 17'sd1;
        y_lo = cf_y_wrap && row < 0;
        y_hi = cf_y_wrap && !row_up[16];
        x_lo = cf_x_wrap && col < 0;
        x_hi = cf_x_wrap && col >= w_s;
        rowmap_settle = fold && (rejected || !(y_lo || y_hi || x_lo || x_hi));
        rowmap_reject = rejected || row < 0 || !row_up[16];
        rowmap_row = row;
        rowmap_col = col;
        rowmap_blank = blank;
        rowmap_row_base = row_base;
    end

    logic left;
    logic [16:0] pad_left, run_w;
    always_comb begin
        pad_left = 17'(-col);
        run_w = 17'(w_s - col);
        left = !blank && col < 0;
        rowmap_run = !blank && !left && col < w_s;
        rowmap_right = !left && !rowmap_run;
        rowmap_seg_px = px_rem;
        if (left) begin
            if (pad_left < {7'd0, px_rem})
                rowmap_seg_px = pad_left[9:0];
        end else if (rowmap_run) begin
            if (run_max < rowmap_seg_px)
                rowmap_seg_px = run_max;
            if (run_w < {7'd0, rowmap_seg_px})
                rowmap_seg_px = run_w[9:0];
        end
        rowmap_seg_valid = seg_on && px_rem != 10'd0
            && (!rowmap_run || run_ready);
        rowmap_wrap = cf_x_wrap
            && col + $signed({7'd0, rowmap_seg_px}) == w_s;
        rowmap_end = seg_take && px_rem == rowmap_seg_px;
    end

    initial begin
        fold = 1'b0;
        row = '0;
        col = '0;
        w_s = '0;
        h_s = '0;
        blank = 1'b0;
        px_rem = '0;
        row_base = '0;
    end
    always_ff @(posedge clk) begin
        if (abort_i)
            fold <= 1'b0;
        else if (start) begin
            row <= 17'($signed(row16));
            col <= 17'($signed(col16));
            w_s <= 17'($signed(16'(cfgw[63:48] * {11'd0, win_wf})));
            h_s <= 17'($signed(16'(cfgw[79:64] * {11'd0, win_hf})));
            blank <= 1'b0;
            fold <= 1'b1;
        end else if (fold) begin
            if (rowmap_settle) begin
                blank <= rowmap_reject;
                fold <= 1'b0;
            end else if (y_lo)
                row <= row + h_s;
            else if (y_hi)
                row <= row_up;
            else if (x_lo)
                col <= col + w_s;
            else
                col <= col - w_s;
        end else begin
            if (addr) begin
                row_base <= {1'b0, cf_data}
                    + 17'(17'(data_row) * 17'(sizeof_row[16:0]));
                px_rem <= cw;
                if (rowmap_overrun)
                    blank <= 1'b1;
            end
            if (seg_take) begin
                px_rem <= px_rem - rowmap_seg_px;
                if (left)
                    col <= col + $signed({7'd0, rowmap_seg_px});
                else if (rowmap_run)
                    col <= rowmap_wrap ? 17'sd0
                        : col + $signed({7'd0, rowmap_seg_px});
            end
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_rowmap;
    always_comb unused_rowmap = ^{pad_left[16:10], run_w[16:10]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
