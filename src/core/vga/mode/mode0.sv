/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The terminal's cell memory and scanout registers, the fabric's copy of
 * what core/vga/mode/mode0.h defines: mode0_cells, and mode0_regs's row,
 * cursor, cursor color and screen words. The linker places both behind
 * this window, so cells.c's ordinary stores land here unchanged, and this
 * draws what mode0.c draws.
 *
 * Firmware writes are shadows; the scanout latches them at frame start,
 * for one frame of latency and never a tear.
 *
 * The cells power up zero — BRAM contents ship in the bitstream — so
 * there is no boot-time clear.
 */

module mode0 (
    input logic clk,
    input logic frame_start,

    input logic [9:0] h,
    input logic [9:0] v,
    input logic px_last,
    input logic line_start,
    input logic [9:0] cw,

    /* The row map, from sched.sv, which pairs the lines of a 320 wide
     * canvas so a row of graphics spans two of them. */
    input logic [9:0] t,
    input logic pair_start,
    input logic pair_end,

    /* sched.sv found the terminal's plane on this row: go starts the row,
     * and cfg is the scanline the terminal begins at. */
    input logic term_go,
    input logic [15:0] term_cfg,
    output logic [15:0] mode0_pix,

    output logic mode0_f_req,
    output logic [13:0] mode0_f_addr,
    input logic f_gnt,
    input logic [7:0] f_data,

    input logic b_stb,
    input logic b_we,
    input logic [16:0] b_addr,
    input logic [3:0] b_wstrb,
    input logic [31:0] b_wdata,
    output logic [31:0] mode0_b_rdata
);

    /* One array per byte lane: a byte-enabled write keeps a true
     * dual-port RAM from being inferred at all, and the lanes cost the
     * same bits.
     *
     * 15360 words is the tallest terminal the engine can build with both
     * screens. This firmware uses 14400; both round to the same depth.
     *
     * Flat rather than banked: hand-banking gives the blocks back more
     * exactly and pays for it with an adder and a mux on the scanout's
     * address. */
    (* ramstyle = "no_rw_check" *)
    logic [7:0] cell0[15360] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] cell1[15360] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] cell2[15360] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] cell3[15360] /*verilator public_flat_rw*/;

    logic [13:0] cell_idx;
    always_comb cell_idx = b_addr[15:2];

    /* row {size[17:16], cell[12:0]};
     * cursor {enabled[25], lit[24], style[23:16], y[15:8], x[7:0]};
     * screen {reverse[2], blink[1:0]}. */
    logic [31:0] cursor_shadow /*verilator public_flat_rd*/;
    logic [15:0] cursor_color_shadow /*verilator public_flat_rd*/;
    logic [2:0] screen_shadow /*verilator public_flat_rw*/;
    logic [31:0] frame_count /*verilator public_flat_rd*/;

    /* The scanout read stands alone and unreset: a block RAM's output
     * register has no asynchronous clear, and a read inside the
     * pipeline's reset would keep the cells out of memory entirely. */
    always_ff @(posedge clk)
        fetch_q <= {cell3[fetch_word], cell2[fetch_word],
                    cell1[fetch_word], cell0[fetch_word]};

    logic [31:0] cells_q, regs_q;
    logic sel_cells;
    always_comb mode0_b_rdata = sel_cells ? cells_q : regs_q;

    logic cell_w0, cell_w1, cell_w2, cell_w3;
    logic [31:0] cell_d;
    always_comb begin
        cell_w0 = b_stb && !b_addr[16] && b_we && b_wstrb[0];
        cell_w1 = b_stb && !b_addr[16] && b_we && b_wstrb[1];
        cell_w2 = b_stb && !b_addr[16] && b_we && b_wstrb[2];
        cell_w3 = b_stb && !b_addr[16] && b_we && b_wstrb[3];
        cell_d = b_wdata;
    end

    always_ff @(posedge clk) begin
        if (b_stb) begin
            cells_q <= {cell3[cell_idx], cell2[cell_idx],
                        cell1[cell_idx], cell0[cell_idx]};
            if (cell_w0) cell0[cell_idx] <= cell_d[7:0];
            if (cell_w1) cell1[cell_idx] <= cell_d[15:8];
            if (cell_w2) cell2[cell_idx] <= cell_d[23:16];
            if (cell_w3) cell3[cell_idx] <= cell_d[31:24];
        end
    end
    /* The firmware reads back only the frame counter, which it can also
     * write, so a savestate puts back the count it was taken at. */
    always_ff @(posedge clk) begin
        if (b_stb) begin
            sel_cells <= !b_addr[16];
            regs_q <= frame_count;
        end
    end

    initial begin
        cursor_shadow = 32'h0;
        cursor_color_shadow = 16'h0;
        screen_shadow = 3'h0;
        frame_count = 32'h0;
    end
    always_ff @(posedge clk) begin
        if (frame_start)
            frame_count <= frame_count + 32'd1;
        if (b_stb && b_we && b_addr[16]) begin
            case (b_addr[7:2])
                6'd32: cursor_shadow <= b_wdata;
                6'd33: cursor_color_shadow <= b_wdata[15:0];
                6'd34: screen_shadow <= b_wdata[2:0];
                6'd40: frame_count <= b_wdata;
                default: ;
            endcase
        end
    end

    /* Latched at the start of the line the render takes row 0 on, so a
     * mid-frame publish never tears: line 524, or 523 on a 320-wide canvas,
     * whose row 0 is the pair (523, 524) and is drawn on its first line. */
    logic frame_render;
    always_comb frame_render = line_start
        && v == (cw == 10'd320 ? 10'd523 : 10'd524);

    /* Each row's word has two slots, and row_front picks the one
     * the render reads. A write goes to the other slot and is flipped in
     * by the latch, which flips only the rows written since the last one,
     * so the table costs a small MLAB rather than two banks of registers.
     * The write is registered first, keeping the bus's half-period
     * address path off the MLAB's port. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [14:0] row_mem[64] /*verilator public_flat_rd*/;
    logic [31:0] row_front /*verilator public_flat_rd*/;
    logic [31:0] row_pend /*verilator public_flat_rd*/;
    logic row_w;
    logic [4:0] row_wi;
    logic [14:0] row_wd;
    initial begin
        for (int i = 0; i < 64; i++)
            row_mem[i] = 15'd0;
        row_front = '0;
        row_pend = '0;
        row_w = 1'b0;
        row_wi = '0;
        row_wd = '0;
    end
    always_ff @(posedge clk)
        if (row_w)
            row_mem[{row_wi, !row_front[row_wi]}] <= row_wd;
    always_ff @(posedge clk) begin
        row_w <= b_stb && b_we && b_addr[16] && !b_addr[7];
        row_wi <= b_addr[6:2];
        row_wd <= {b_wdata[17:16], b_wdata[12:0]};
        if (frame_render) begin
            row_front <= row_front
                ^ (row_pend | (32'(row_w) << row_wi));
            row_pend <= '0;
        end else if (row_w)
            row_pend[row_wi] <= 1'b1;
    end
    logic [14:0] row_word;
    always_comb row_word = row_mem[{logical_row, row_front[logical_row]}];

    logic [31:0] cursor_q;
    logic [15:0] cursor_color_q;
    logic [1:0] blink_q;
    logic reverse_q;
    initial begin
        cursor_q = 32'h0;
        cursor_color_q = 16'h0;
        blink_q = 2'h0;
        reverse_q = 1'b0;
    end
    always_ff @(posedge clk) begin
        if (frame_render) begin
            cursor_q <= cursor_shadow;
            cursor_color_q <= cursor_color_shadow;
            blink_q <= screen_shadow[1:0];
            reverse_q <= screen_shadow[2];
        end
    end

    /* Eight clocks a cell against a line of 800; the margin is why the
     * fetches can be serial. The bank rides in the address, because a
     * bank index outside it reads both halves and muxes, which is an
     * asynchronous read and no block RAM at all. */
    (* ramstyle = "no_rw_check" *)
    logic [15:0] linebuf[2048];

    logic wr_bank;
    logic [8:0] term_line;
    logic [4:0] logical_row;
    logic [3:0] scanrow;
    logic [3:0] glyph_row;
    logic [1:0] size_r;  // the row's TERM_LINE_ size
    logic wide;
    logic [7:0] line_mask;
    logic ul_row;
    logic cur_hit;  // cursor overlay lands on this line
    logic [6:0] cur_cx;
    logic [2:0] cur_style;  // wrap-park forces block

    logic [9:0] px;    // write pointer into the line
    logic [3:0] step;
    logic run /*verilator public_flat_rd*/;

    logic [31:0] w0_n;
    logic [13:0] fetch_word;
    logic [31:0] fetch_q;
    logic [15:0] fg_r, bg_r;
    logic [7:0] shreg;

    logic [1:0] font_sel;
    logic [7:0] font_code;
    logic [7:0] font_bits;

    always_comb begin
        if (w0_n[14])  /* ATTR_DEC */
            font_sel = 2'd1;
        else if (!use_40 && w0_n[15] && !w0_n[7])  /* ATTR_ITALIC, 8x16 only */
            font_sel = 2'd2;
        else
            font_sel = 2'd0;
        font_code = font_sel == 2'd1 ? w0_n[7:0] - 8'h5F : w0_n[7:0];
    end
    /* The cell's word lands seven clocks before its glyph is needed,
     * fifteen on a double-width row, so the request stands and the byte is
     * there in time. */
    always_comb begin
        case (font_sel)
            2'd1: mode0_f_addr = use_40
                ? {2'b11, 3'b001, 1'b0, glyph_row[2:0], font_code[4:0]}
                : {2'b11, 3'b000, glyph_row, font_code[4:0]};
            2'd2: mode0_f_addr = {2'b10, 1'b0, glyph_row, font_code[6:0]};
            default: mode0_f_addr = use_40
                ? {2'b01, 1'b0, glyph_row[2:0], font_code}
                : {2'b00, glyph_row, font_code};
        endcase
        mode0_f_req = run;
    end

    /* The store answers the clock the resolve wants it, so the arriving
     * byte passes straight through. */
    logic f_gnt_d;
    logic [7:0] font_hold;
    always_ff @(posedge clk) begin
        f_gnt_d <= f_gnt;
        if (f_gnt_d)
            font_hold <= f_data;
    end
    always_comb font_bits = f_gnt_d ? f_data : font_hold;

    /* 320-wide canvases run the 40-column terminal: 8x8 cells, so one
     * fewer row bit everywhere and the 8-row attribute lines.
     *
     * Every line size but the normal one is double width: half the row's
     * cells, each pixel twice across. The top and bottom halves of a
     * double-height line draw each glyph row of their half twice, and the
     * attribute lines follow the glyph row, while the cursor's underline
     * follows the scanline. */
    logic use_40;
    always_comb use_40 = cw == 10'd320;
    always_comb wide = size_r != 2'd0;  /* TERM_LINE_NORMAL */
    always_comb begin
        if (use_40) begin
            scanrow = {1'b0, term_line[2:0]};
            logical_row = term_line[7:3];
            case (size_r)
                2'd2: glyph_row = {2'b00, scanrow[2:1]};  /* TERM_LINE_TOP */
                2'd3: glyph_row = {2'b01, scanrow[2:1]};  /* TERM_LINE_BOTTOM */
                default: glyph_row = scanrow;
            endcase
            line_mask = {2'b00,
                         glyph_row == 4'd0,                    /* ATTR_OVERLINE */
                         glyph_row == 4'd4,                    /* ATTR_STRIKE */
                         glyph_row == 4'd7 || glyph_row == 4'd5, /* ATTR_DBL_UL */
                         glyph_row == 4'd7,                    /* ATTR_UNDERLINE */
                         2'b00};
            ul_row = glyph_row == 4'd7 || glyph_row == 4'd5;
        end else begin
            scanrow = term_line[3:0];
            logical_row = term_line[8:4];
            case (size_r)
                2'd2: glyph_row = {1'b0, scanrow[3:1]};
                2'd3: glyph_row = {1'b1, scanrow[3:1]};
                default: glyph_row = scanrow;
            endcase
            line_mask = {2'b00,
                         glyph_row == 4'd0,                      /* ATTR_OVERLINE */
                         glyph_row == 4'd8,                      /* ATTR_STRIKE */
                         glyph_row == 4'd15 || glyph_row == 4'd13, /* ATTR_DBL_UL */
                         glyph_row == 4'd15,                     /* ATTR_UNDERLINE */
                         2'b00};
            ul_row = glyph_row == 4'd15 || glyph_row == 4'd13;
        end
    end

    /* These steps run in the C renderer's order: under DECSCNM every
     * cell's colours swap, a blinking cell's glyph is drawn in the
     * background colour while its blink phase bit is set unless the block
     * cursor is drawn on the cell, every pixel is set on a row where an
     * overline, strike or underline is drawn, an underline row takes the
     * underline colour, and then under the block cursor the glyph is drawn
     * in the cell's background colour on the cursor colour. The cell's
     * second word is taken straight from the scanout read, which holds it
     * on both clocks the resolve is used: step 5 and each cell's last
     * pixel. */
    logic cur_here, cur_block;
    logic [7:0] attr_r;
    logic [7:0] bits_res;
    logic [15:0] cell_fg, cell_bg;
    logic [15:0] fg_res, bg_res;
    always_comb begin
        attr_r = w0_n[15:8];
        /* The cell being resolved is one ahead of the one px is in. */
        cur_here = cur_hit && (wide ? 7'((px + 10'd1) >> 4)
                                    : 7'((px + 10'd1) >> 3)) == cur_cx;
        cur_block = cur_here && (cur_style == 3'd0 || cur_style == 3'd1
                                 || cur_style == 3'd2);
        cell_fg = reverse_q ? fetch_q[15:0] : w0_n[31:16];
        cell_bg = reverse_q ? w0_n[31:16] : fetch_q[15:0];
        bits_res = font_bits;
        fg_res = cell_fg;
        bg_res = cell_bg;
        if (!cur_block && (attr_r & {6'b0, blink_q}) != 8'h00)
            fg_res = cell_bg;
        if ((attr_r & line_mask) != 8'h00) begin
            bits_res = 8'hFF;
            if (ul_row && !cur_block)
                fg_res = fetch_q[31:16];
        end
        if (cur_block) begin
            fg_res = cell_bg;
            bg_res = cursor_color_q;
        end
        if (cur_here && (cur_style == 3'd3 || cur_style == 3'd4)
            && (use_40 ? scanrow == 4'd7
                       : scanrow == 4'd14 || scanrow == 4'd15)) begin
            bits_res = 8'hFF;
            fg_res = cursor_color_q;
            bg_res = cursor_color_q;
        end
    end


    /* A cell is 8 clocks, or 16 on a double-width row, where each glyph
     * bit is written twice. phase counts the clocks of the cell px is in. */
    logic [3:0] phase, phase_last;
    always_comb begin
        phase = wide ? px[3:0] : {1'b0, px[2:0]};
        phase_last = wide ? 4'd15 : 4'd7;
    end

    /* The cursor's column on this row: half as many on a double-width
     * row, and a pending wrap parks it on the last, as a block. */
    logic [7:0] cols_row;
    always_comb cols_row = row_word[14:13] != 2'd0
        ? (use_40 ? 8'd20 : 8'd40) : (use_40 ? 8'd40 : 8'd80);

    initial begin
        wr_bank = 1'b0;
        run = 1'b0;
        term_line = '0;
        size_r = '0;
        cur_hit = 1'b0;
        cur_cx = '0;
        cur_style = '0;
        px = '0;
        step = '0;
        w0_n = '0;
        fetch_word = '0;
        fg_r = '0;
        bg_r = '0;
        shreg = '0;
    end
    always_ff @(posedge clk) begin
        if (line_start) begin
            if (pair_start)
                wr_bank <= !wr_bank;
        end else if (term_go) begin
            run <= 1'b1;
            px <= '0;
            term_line <= t[8:0] - term_cfg[8:0];
            step <= 4'd1;
        end else if (run) begin
            case (step)
                4'd1: begin
                    size_r <= row_word[14:13];
                    cur_hit <= cursor_q[25]  /* enabled */
                        && (cursor_q[24]     /* lit, or steady style */
                            || cursor_q[18:16] == 3'd2
                            || cursor_q[18:16] == 3'd4
                            || cursor_q[18:16] == 3'd6)
                        && cursor_q[15:8] == {3'd0, logical_row};
                    cur_cx <= cursor_q[7:0] >= cols_row
                        ? 7'(cols_row - 8'd1) : cursor_q[6:0];
                    cur_style <= cursor_q[7:0] >= cols_row
                        ? 3'd1 : cursor_q[18:16];
                    fetch_word <= {row_word[12:0], 1'b0};
                    step <= 4'd2;
                end
                4'd2: begin
                    fetch_word <= fetch_word + 14'd1;
                    step <= 4'd3;
                end
                4'd3: begin
                    w0_n <= fetch_q;
                    step <= 4'd4;
                end
                4'd4: begin
                    fetch_word <= fetch_word + 14'd1;
                    step <= 4'd5;
                end
                4'd5: begin
                    fg_r <= fg_res;
                    bg_r <= bg_res;
                    shreg <= bits_res;
                    fetch_word <= fetch_word + 14'd1;
                    step <= 4'd6;
                end
                default: begin
                    if (!wide || px[0])
                        shreg <= {shreg[6:0], 1'b0};
                    px <= px + 10'd1;
                    if (phase == 4'd0)
                        w0_n <= fetch_q;
                    if (phase == phase_last - 4'd1)
                        fetch_word <= fetch_word + 14'd1;
                    if (phase == phase_last) begin
                        fg_r <= fg_res;
                        bg_r <= bg_res;
                        shreg <= bits_res;
                        fetch_word <= fetch_word + 14'd1;
                        if (px == (use_40 ? 10'd319 : 10'd639))
                            run <= 1'b0;
                    end
                end
            endcase
        end
    end

    /* The bar cursor patches the cell being written out, which is one
     * behind the cell being resolved. */
    logic cur_bar_out;
    always_comb cur_bar_out = cur_hit
        && (wide ? px[9:4] == cur_cx[5:0] && cur_cx[6] == 1'b0
                 : px[9:3] == cur_cx)
        && (cur_style == 3'd5 || cur_style == 3'd6);

    /* Its own unreset block: a port inside the pipeline's reset keeps
     * the whole buffer out of memory. The write is the pipeline's last
     * step, so it runs on the clocks that step does. */
    always_ff @(posedge clk)
        if (!line_start && run && step >= 4'd6)
            linebuf[{wr_bank, px}] <=
                (cur_bar_out && phase < (use_40 ? 4'd1 : 4'd2))
                    ? cursor_color_q
                    : (shreg[7] ? fg_r : bg_r);

    /* The bank toggle lands on h==0's first tick, so only the pixel-0
     * read at the end of h==799 still sees the line under its write-side
     * label. */
    logic [10:0] lb_rd;
    always_comb lb_rd = h == 10'd799
        ? {pair_end ? wr_bank : !wr_bank, 10'd0}
        : {!wr_bank, 10'(h + 10'd1)};

    /* The buffer's output register is loaded from nothing but the
     * buffer, because a branch handing it a constant turns the read into
     * combinational logic and a mux in the fabric, and the line buffer
     * then cannot be placed in block memory. */
    logic [15:0] lb_q;
    logic lb_blank;
    always_ff @(posedge clk) begin
        if (px_last) begin
            lb_q <= linebuf[lb_rd];
            lb_blank <= !(h == 10'd799
                          || h < (use_40 ? 10'd319 : 10'd639));
        end
    end
    always_comb mode0_pix = lb_blank ? 16'h0000 : lb_q;

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_mode0;
    always_comb unused_mode0 = ^{b_addr[1:0], term_cfg[15:9],
                                    cursor_q[31:26], cursor_q[23:19], t[9]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
