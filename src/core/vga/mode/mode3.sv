/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 3, the linear bitmap of core/vga/mode/mode3.c: rows mapped with true
 * wraparound, the oracle's rejects (range, bitmap overrun, the 16bpp odd
 * row), and the line described to the shared pixel tail as segments. The
 * tail owns the fetching, slicing, palette and pixels; this front owns
 * the geometry. A wrapped bitmap is runs of the bitmap's width back to
 * back; a clipped one is a run with padding around it; a rejected line
 * is one padding segment.
 */

module mode3 (
    input logic clk,

    /* One line of work: start when the config view is valid; abort_i is
     * the next line's deadline. */
    input logic start,
    input logic abort_i,
    input logic [15:0] attr,
    input logic [111:0] cfgw,
    input logic [8:0] t_row,
    input logic [9:0] cw,

    output logic mode3_tl_start,
    output logic [15:0] mode3_pal_ptr,
    output logic mode3_pal_xram,
    output logic [2:0] mode3_bpp,
    output logic mode3_reversed,
    output logic mode3_seg_valid,
    output logic mode3_seg_imm,
    output logic [22:0] mode3_seg_bits,
    output logic [9:0] mode3_seg_px,
    input logic seg_take
);

    logic cf_x_wrap, cf_y_wrap;
    logic signed [15:0] cf_x_pos, cf_y_pos, cf_width, cf_height;
    logic [15:0] cf_data, cf_palette;
    always_comb begin
        cf_x_wrap = cfgw[7:0] != 8'h00;
        cf_y_wrap = cfgw[15:8] != 8'h00;
        cf_x_pos = cfgw[31:16];
        cf_y_pos = cfgw[47:32];
        cf_width = cfgw[63:48];
        cf_height = cfgw[79:64];
        cf_data = cfgw[95:80];
        cf_palette = cfgw[111:96];
    end

    /* Attribute code to depth; 8-10 are the reversed 1/2/4. */
    logic reversed;
    logic [2:0] bpp_log;
    always_comb begin
        reversed = attr[3];
        case (attr[2:0])
            3'd0: bpp_log = 3'd0;
            3'd1: bpp_log = 3'd1;
            3'd2: bpp_log = 3'd2;
            3'd3: bpp_log = 3'd3;
            default: bpp_log = 3'd4;
        endcase
    end

    typedef enum logic [1:0] {
        S3_IDLE, S3_WRAP, S3_ADDR, S3_SEG
    } state_t;
    state_t state;

    logic signed [16:0] row;
    logic [19:0] sizeof_row;
    logic [19:0] row_off;
    logic [16:0] row_base;
    logic signed [16:0] col;
    logic [9:0] px_rem;
    logic blank;

    /* The oracle stores these in int16, so ±32768 wraps before the
     * wraparound fold sees it. */
    logic [15:0] row16, col16;
    always_comb row16 = {7'd0, t_row} - 16'(cf_y_pos);
    always_comb col16 = 16'd0 - 16'(cf_x_pos);

    logic signed [16:0] width_s;
    always_comb width_s = $signed({cf_width[15], cf_width});
    logic [16:0] pad_left;
    always_comb pad_left = 17'(-col);
    logic [16:0] run_w;
    always_comb run_w = 17'(width_s - col);
    always_comb begin
        mode3_seg_valid = state == S3_SEG && px_rem != 10'd0;
        mode3_seg_imm = 1'b1;
        mode3_seg_bits = 23'd0;
        mode3_seg_px = px_rem;
        if (!blank && col < 0) begin
            if (pad_left < {7'd0, px_rem})
                mode3_seg_px = pad_left[9:0];
        end else if (!blank && col < width_s) begin
            mode3_seg_imm = 1'b0;
            mode3_seg_bits = ({6'd0, row_base} << 3)
                + (23'(col[15:0]) << bpp_log);
            if (run_w < {7'd0, px_rem})
                mode3_seg_px = run_w[9:0];
        end
        /* else: blank, or right padding — the rest of the line. */
    end

    initial begin
        state = S3_IDLE;
        row = '0;
        sizeof_row = '0;
        row_off = '0;
        row_base = '0;
        col = '0;
        px_rem = '0;
        blank = 1'b0;
        mode3_tl_start = 1'b0;
        mode3_pal_ptr = '0;
        mode3_pal_xram = 1'b0;
        mode3_bpp = '0;
        mode3_reversed = 1'b0;
    end
    always_ff @(posedge clk) begin
        mode3_tl_start <= 1'b0;
        if (abort_i) begin
`ifdef VERILATOR
            if (state == S3_WRAP || state == S3_ADDR)
                $fatal(1, "mode3 underrun");
`endif
            state <= S3_IDLE;
        end else if (start) begin
            row <= $signed({row16[15], row16});
            sizeof_row <= ((20'(cf_width) << bpp_log) + 20'd7) >> 3;
            col <= $signed({col16[15], col16});
            state <= S3_WRAP;
        end else begin
            case (state)
                S3_IDLE: ;
                S3_WRAP: begin
                    /* Iterative wraparound; sane configs settle in a
                     * step or two, and the beam's deadline bounds the
                     * pathological ones. */
                    if (cf_width < 16'sd1 || cf_height < 16'sd1) begin
                        blank <= 1'b1;
                        state <= S3_ADDR;
                    end else if (cf_y_wrap && row < 0)
                        row <= row + {cf_height[15], cf_height};
                    else if (cf_y_wrap
                             && row >= $signed({cf_height[15], cf_height}))
                        row <= row - {cf_height[15], cf_height};
                    else if (cf_x_wrap && col < 0)
                        col <= col + {cf_width[15], cf_width};
                    else if (cf_x_wrap
                             && col >= $signed({cf_width[15], cf_width}))
                        col <= col - {cf_width[15], cf_width};
                    else if (row < 0
                             || row >= $signed({cf_height[15], cf_height}))
                    begin
                        blank <= 1'b1;
                        state <= S3_ADDR;
                    end else begin
                        blank <= 1'b0;
                        row_off <= 20'(37'(row[15:0])
                                       * 37'(sizeof_row));
                        state <= S3_ADDR;
                    end
                end
                S3_ADDR: begin
                    row_base <= {1'b0, cf_data} + row_off[16:0];
                    /* Bitmap overrun, and 16bpp rejects an odd row. */
                    if (!blank
                        && (35'(cf_height[14:0]) * 35'(sizeof_row)
                            > 35'(17'h10000) - 35'({1'b0, cf_data})
                            || (bpp_log == 3'd4
                                && (cf_data[0] ^ row_off[0]))))
                        blank <= 1'b1;
                    /* The tail's plan: a blank line loads nothing.
                     * The pal_xram test folds the blank decision in
                     * combinationally, since blank may land on this
                     * same edge. */
                    mode3_pal_ptr <= cf_palette;
                    mode3_pal_xram <= !blank
                        && !(35'(cf_height[14:0]) * 35'(sizeof_row)
                             > 35'(17'h10000) - 35'({1'b0, cf_data})
                             || (bpp_log == 3'd4
                                 && (cf_data[0] ^ row_off[0])))
                        && !cf_palette[0]
                        && bpp_log != 3'd4
                        && {1'b0, cf_palette}
                            <= 17'h10000
                                - (17'd2 << {12'd0, 5'd1 << bpp_log});
                    mode3_bpp <= bpp_log;
                    mode3_reversed <= reversed;
                    mode3_tl_start <= 1'b1;
                    px_rem <= cw;
                    state <= S3_SEG;
                end
                S3_SEG: begin
                    if (seg_take) begin
                        px_rem <= px_rem - mode3_seg_px;
                        if (!blank && col < 0)
                            col <= col + $signed({7'd0,
                                                  mode3_seg_px});
                        else if (!blank && col < width_s)
                            col <= cf_x_wrap ? 17'sd0
                                : col + $signed({7'd0,
                                                 mode3_seg_px});
                        if (px_rem == mode3_seg_px)
                            state <= S3_IDLE;
                    end
                end
                default: state <= S3_IDLE;
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_mode3;
    always_comb unused_mode3 = ^{row_off[19:17], sizeof_row, cfgw,
                                     attr[15:4], pad_left[16:10],
                                     run_w[16:10]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
