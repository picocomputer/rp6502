/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The terminal's memories and scanout registers, the fabric's copy of
 * what core/vga/mode/mode0.h defines: mode0_cells, mode0_colors,
 * mode0_palette, and mode0_regs's row, cursor, cursor color and screen
 * words. The linker places each behind its window, so cells.c's ordinary
 * stores land here unchanged, and this draws what mode0.c draws.
 *
 * There is one terminal, so this is one engine of its own beside the
 * fill engine, with its own line buffer. It has a line to finish in and
 * takes nothing from the fills.
 *
 * Firmware writes to the registers are shadows; the scanout latches them
 * as a frame starts, for one frame of latency and never a tear. A publish
 * is many writes, closed by a write to the publish word, and a frame that
 * starts while one is open keeps the last frame's words.
 *
 * A smooth scroll (DECSCLM) moves its rows a scanline a frame over the
 * row words, which already have the rows in their new places, as
 * core/vga/mode/mode0.c draws it.
 *
 * The memories power up zero, as BRAM contents ship in the bitstream, so
 * there is no boot-time clear.
 */

module mode0 (
    input logic clk,
    input logic frame_start,

    input logic [9:0] h,
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

    /* The soft CPU's windows: b_sel 0 is the cells, 1 the registers, 2
     * the colors and 3 the palette. */
    input logic b_stb,
    input logic b_we,
    input logic [1:0] b_sel,
    input logic [15:0] b_addr,
    input logic [3:0] b_wstrb,
    input logic [31:0] b_wdata,
    output logic [31:0] mode0_b_rdata
);

    /* One array per byte lane: a byte-enabled write keeps a true
     * dual-port RAM from being inferred at all, and the lanes cost the
     * same bits.
     *
     * The cells are mode0_cells, a word each, and the colors are
     * mode0_colors as the firmware has them: a run of halfwords, three a
     * cell, so a cell's colors start on a word or in the middle of one.
     * The palette is mode0_palette, two entries a word. The depths are
     * this firmware's, MODE0_CELLS of 7440.
     *
     * Flat rather than banked: hand-banking gives the blocks back more
     * exactly and pays for it with an adder and a mux on the scanout's
     * address. */
    (* ramstyle = "no_rw_check" *)
    logic [7:0] cell0[7440] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] cell1[7440] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] cell2[7440] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] cell3[7440] /*verilator public_flat_rw*/;

    (* ramstyle = "no_rw_check" *)
    logic [7:0] col0[11160] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] col1[11160] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] col2[11160] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] col3[11160] /*verilator public_flat_rw*/;

    (* ramstyle = "no_rw_check" *)
    logic [7:0] pal0[258] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] pal1[258] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] pal2[258] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] pal3[258] /*verilator public_flat_rw*/;

    logic [12:0] cell_idx;
    logic [13:0] col_idx;
    logic [8:0] pal_idx;
    always_comb begin
        cell_idx = b_addr[14:2];
        col_idx = b_addr[15:2];
        pal_idx = b_addr[10:2];
    end

    /* row {size[17:16], cell[12:0]};
     * cursor {enabled[25], lit[24], style[23:16], y[15:8], x[7:0]};
     * screen {reverse[2], blink[1:0]};
     * smooth {on[17], up[16], bot[15:8], top[7:0]};
     * smooth_frame, a frame count; smooth_ghost, a row word. */
    logic [31:0] cursor_shadow /*verilator public_flat_rd*/;
    logic [15:0] cursor_color_shadow /*verilator public_flat_rd*/;
    logic [2:0] screen_shadow /*verilator public_flat_rw*/;
    logic sm_on_s, sm_up_s;
    logic [4:0] sm_top_s, sm_bot_s;
    logic [31:0] sm_frame_s;
    logic [14:0] sm_ghost_s;
    logic [31:0] frame_count /*verilator public_flat_rd*/;

    /* The scanout reads stand alone and unreset: a block RAM's output
     * register has no asynchronous clear, and a read inside the
     * pipeline's reset would keep a memory out of block RAM entirely. */
    logic [12:0] cell_ra;
    logic [13:0] col_ra;
    logic [9:0] pal_ra;
    logic [31:0] cell_rq, col_rq, pal_rq;
    logic pal_half;
    always_ff @(posedge clk)
        cell_rq <= {cell3[cell_ra], cell2[cell_ra],
                    cell1[cell_ra], cell0[cell_ra]};
    always_ff @(posedge clk)
        col_rq <= {col3[col_ra], col2[col_ra],
                   col1[col_ra], col0[col_ra]};
    always_ff @(posedge clk) begin
        pal_rq <= {pal3[pal_ra[9:1]], pal2[pal_ra[9:1]],
                   pal1[pal_ra[9:1]], pal0[pal_ra[9:1]]};
        pal_half <= pal_ra[0];
    end

    logic [31:0] cells_q, cols_q, pals_q, regs_q;
    logic [1:0] sel_q;
    always_comb begin
        case (sel_q)
            2'd0: mode0_b_rdata = cells_q;
            2'd2: mode0_b_rdata = cols_q;
            2'd3: mode0_b_rdata = pals_q;
            default: mode0_b_rdata = regs_q;
        endcase
    end

    logic cell_stb, col_stb, pal_stb;
    always_comb begin
        cell_stb = b_stb && b_sel == 2'd0;
        col_stb = b_stb && b_sel == 2'd2;
        pal_stb = b_stb && b_sel == 2'd3;
    end

    always_ff @(posedge clk) begin
        if (cell_stb) begin
            cells_q <= {cell3[cell_idx], cell2[cell_idx],
                        cell1[cell_idx], cell0[cell_idx]};
            if (b_we && b_wstrb[0]) cell0[cell_idx] <= b_wdata[7:0];
            if (b_we && b_wstrb[1]) cell1[cell_idx] <= b_wdata[15:8];
            if (b_we && b_wstrb[2]) cell2[cell_idx] <= b_wdata[23:16];
            if (b_we && b_wstrb[3]) cell3[cell_idx] <= b_wdata[31:24];
        end
    end
    always_ff @(posedge clk) begin
        if (col_stb) begin
            cols_q <= {col3[col_idx], col2[col_idx],
                       col1[col_idx], col0[col_idx]};
            if (b_we && b_wstrb[0]) col0[col_idx] <= b_wdata[7:0];
            if (b_we && b_wstrb[1]) col1[col_idx] <= b_wdata[15:8];
            if (b_we && b_wstrb[2]) col2[col_idx] <= b_wdata[23:16];
            if (b_we && b_wstrb[3]) col3[col_idx] <= b_wdata[31:24];
        end
    end
    always_ff @(posedge clk) begin
        if (pal_stb) begin
            pals_q <= {pal3[pal_idx], pal2[pal_idx],
                       pal1[pal_idx], pal0[pal_idx]};
            if (b_we && b_wstrb[0]) pal0[pal_idx] <= b_wdata[7:0];
            if (b_we && b_wstrb[1]) pal1[pal_idx] <= b_wdata[15:8];
            if (b_we && b_wstrb[2]) pal2[pal_idx] <= b_wdata[23:16];
            if (b_we && b_wstrb[3]) pal3[pal_idx] <= b_wdata[31:24];
        end
    end
    /* A write to any of the words a frame copies opens a publish, and the
     * publish word, written last, closes it. */
    logic pub_open /*verilator public_flat_rd*/;
    /* A frame started while the publish was open, so that frame kept the
     * last frame's words. The publish word reads back 0 then, and 1 once
     * the publish was taken whole, as a machine's memory reads back the 1
     * written. The publish's first write clears it. */
    logic pub_missed;
    /* Of the registers the firmware reads back the publish word, whether
     * the last publish was taken whole, and the frame counter, which it
     * can also write, so a savestate puts back the count it was taken
     * at. */
    always_ff @(posedge clk) begin
        if (b_stb) begin
            sel_q <= b_sel;
            regs_q <= b_addr[7:2] == 6'd38 ? {31'b0, !pub_missed} : frame_count;
        end
    end

    logic use_40;  // the 40-column terminal, on a 320-wide canvas
    logic reg_w;
    always_comb reg_w = b_stb && b_we && b_sel == 2'd1;
    initial begin
        cursor_shadow = 32'h0;
        cursor_color_shadow = 16'h0;
        screen_shadow = 3'h0;
        sm_on_s = 1'b0;
        sm_up_s = 1'b0;
        sm_top_s = '0;
        sm_bot_s = '0;
        sm_frame_s = '0;
        sm_ghost_s = '0;
        frame_count = 32'h0;
        pub_open = 1'b0;
        pub_missed = 1'b0;
    end
    always_ff @(posedge clk) begin
        if (frame_start)
            frame_count <= frame_count + 32'd1;
        if (reg_w) begin
            case (b_addr[7:2])
                6'd32: cursor_shadow <= b_wdata;
                6'd33: cursor_color_shadow <= b_wdata[15:0];
                6'd34: screen_shadow <= b_wdata[2:0];
                6'd35: begin
                    sm_top_s <= b_wdata[4:0];
                    sm_bot_s <= b_wdata[12:8];
                    sm_up_s <= b_wdata[16];
                    sm_on_s <= b_wdata[17];
                end
                6'd36: sm_frame_s <= b_wdata;
                6'd37: sm_ghost_s <= {b_wdata[17:16], b_wdata[12:0]};
                6'd40: frame_count <= b_wdata;
                default: ;
            endcase
            if (b_addr[7:2] == 6'd38)
                pub_open <= 1'b0;
            else if (b_addr[7:2] < 6'd38)
                pub_open <= 1'b1;
            if (b_addr[7:2] < 6'd38 && !pub_open)
                pub_missed <= 1'b0;
        end
        if (frame_start && pub_open)
            pub_missed <= 1'b1;
    end

    /* Latched as the frame starts, on the line row 0 is drawn on, with
     * the frame counter's step, unless a publish is part way through. */
    logic frame_render;
    always_comb frame_render = frame_start && !pub_open;

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
        row_w <= reg_w && !b_addr[7];
        row_wi <= b_addr[6:2];
        row_wd <= {b_wdata[17:16], b_wdata[12:0]};
        /* A row written lands a clock after its write opened a publish,
         * so the latch never meets one landing. */
        if (frame_render) begin
            row_front <= row_front ^ row_pend;
            row_pend <= '0;
        end else if (row_w)
            row_pend[row_wi] <= 1'b1;
    end
    logic ghost_r;  // the line is in the row moving out
    logic [14:0] row_word;
    always_comb row_word = ghost_r ? sm_ghost_q
        : row_mem[{logical_row, row_front[logical_row]}];

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

    /* The smooth scroll this frame, as mode0.c's mode0_smooth_moved has
     * it: the scanlines it has moved by this frame's count, one a frame on
     * 16-pixel rows and one every second frame on 8, and ch once it is
     * over; a frame before its first moves 0. It is worked out on the
     * clock after the frame starts, when the count has stepped, and every
     * frame, because a frame that keeps the last words still moves on. */
    logic sm_on_q, sm_up_q;
    logic [4:0] sm_top_q, sm_bot_q;
    logic [31:0] sm_frame_q;
    logic [14:0] sm_ghost_q;
    logic sm_calc;
    logic sm_act;        // a scroll is part way across this frame
    logic [4:0] sm_off;  // scanlines its rows are drawn on from
    logic [31:0] sm_d;
    logic [4:0] sm_moved;
    always_comb begin
        sm_d = frame_count - sm_frame_q;
        if (sm_d[31])
            sm_moved = 5'd0;
        else if (use_40)
            sm_moved = sm_d[30:4] != '0 || sm_d[3:1] == 3'd7
                ? 5'd8 : 5'(sm_d[3:1]) + 5'd1;
        else
            sm_moved = sm_d[30:4] != '0 || sm_d[3:0] == 4'd15
                ? 5'd16 : 5'(sm_d[3:0]) + 5'd1;
    end
    initial begin
        sm_on_q = 1'b0;
        sm_up_q = 1'b0;
        sm_top_q = '0;
        sm_bot_q = '0;
        sm_frame_q = '0;
        sm_ghost_q = '0;
        sm_calc = 1'b0;
        sm_act = 1'b0;
        sm_off = '0;
    end
    always_ff @(posedge clk) begin
        sm_calc <= frame_start;
        if (frame_render) begin
            sm_on_q <= sm_on_s;
            sm_up_q <= sm_up_s;
            sm_top_q <= sm_top_s;
            sm_bot_q <= sm_bot_s;
            sm_frame_q <= sm_frame_s;
            sm_ghost_q <= sm_ghost_s;
        end
        if (sm_calc) begin
            sm_act <= sm_on_q && sm_moved < (use_40 ? 5'd8 : 5'd16);
            sm_off <= sm_up_q ? sm_moved
                : (use_40 ? 5'd8 : 5'd16) - sm_moved;
        end
    end

    /* A cell is eight clocks, and everything a cell is drawn from is
     * fetched in the eight of the cell before it: its word, the two words
     * its colors are in, its glyph's row from the font store, and a
     * palette entry for each color that names one. Each memory is read a
     * clock at a time with its address in a register, so no fetch has
     * logic ahead of it.
     *
     * The bank rides in the line buffer's address, because a bank index
     * outside it reads both halves and muxes, which is an asynchronous
     * read and no block RAM at all. */
    (* ramstyle = "no_rw_check" *)
    logic [15:0] linebuf[2048];

    logic wr_bank;
    logic [8:0] term_line;
    logic [4:0] logical_row;
    logic [3:0] scanrow;
    logic [3:0] glyph_row;
    logic [1:0] size_r;  // the row's TERM_LINE_ size
    logic wide;
    logic strike_row, over_row;
    logic [1:0] ul_row;  // 1 to 3 on the underlines' rows
    logic cur_hit;  // cursor overlay lands on this line
    logic [6:0] cur_cx;
    logic [2:0] cur_style;

    logic [9:0] px;    // write pointer into the line
    logic run /*verilator public_flat_rd*/;
    logic remap;       // the row's first clock, which places it
    logic setup;       // the clock that reads its row word
    logic pre;         // the first cell's fetch, before any pixel

    /* 320-wide canvases run the 40-column terminal: 8x8 cells, so one
     * fewer row bit everywhere.
     *
     * Every line size but the normal one is double width: half the row's
     * cells, each pixel twice across. The top and bottom halves of a
     * double-height line draw each glyph row of their half twice, and the
     * attribute lines follow the glyph row, while the cursor's underline
     * follows the scanline. The underlines are on the glyph's last three
     * rows, the strike on its middle row and the overline on its first. */
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
            strike_row = glyph_row == 4'd4;
            ul_row = glyph_row >= 4'd5 ? 2'(glyph_row - 4'd4) : 2'd0;
        end else begin
            scanrow = term_line[3:0];
            logical_row = term_line[8:4];
            case (size_r)
                2'd2: glyph_row = {1'b0, scanrow[3:1]};
                2'd3: glyph_row = {1'b1, scanrow[3:1]};
                default: glyph_row = scanrow;
            endcase
            strike_row = glyph_row == 4'd8;
            ul_row = glyph_row >= 4'd13 ? 2'(glyph_row - 4'd12) : 2'd0;
        end
        over_row = glyph_row == 4'd0;
    end

    /* core/vga/mode/mode0.c's mode0_underline16 and mode0_underline8: the
     * pixels of each TERM_UL_ style on each of the underlines' rows. */
    function automatic logic [7:0] underline(input logic rows8,
                                             input logic [1:0] row,
                                             input logic [2:0] style);
        case ({row, style})
            {2'd1, 3'd2}: underline = 8'hFF;
            {2'd1, 3'd3}: underline = rows8 ? 8'h00 : 8'h83;
            {2'd2, 3'd3}: underline = rows8 ? 8'h66 : 8'h44;
            {2'd3, 3'd1}: underline = 8'hFF;
            {2'd3, 3'd2}: underline = 8'hFF;
            {2'd3, 3'd3}: underline = rows8 ? 8'h99 : 8'h38;
            {2'd3, 3'd4}: underline = 8'hAA;
            {2'd3, 3'd5}: underline = 8'h7C;
            default: underline = 8'h00;
        endcase
    endfunction

    /* The fetch of one cell, a clock a step:
     *   0  the cell's word and its colors' first word are being read
     *   1  the cell's word and the first color word land
     *   2  the second color word lands; the font is asked for the glyph
     *   3  the foreground's palette entry is being read
     *   4  the foreground lands; the background's entry is being read
     *   5  the background lands; the underline's entry is being read
     *   6  the underline color lands
     *   7  the cell is ready, and waits for the one before it to end
     * The font store is shared with mode 1 and answers on the clock asked
     * or the next, so the glyph's row is in by step 5. */
    logic [2:0] fs;
    logic [14:0] hw;   // the cell's colors, a halfword address
    logic [15:0] glyph_r, attr_r;
    logic [31:0] cw0;
    logic [15:0] c_fg, c_bg, c_ul;  // as the cell has them
    logic [15:0] r_fg, r_bg, r_ul;  // as pixels
    logic f_sent, f_gnt_d;
    logic [7:0] font_r;

    always_comb begin
        mode0_f_req = run && !setup && fs >= 3'd2 && fs != 3'd7 && !f_sent;
        mode0_f_addr = use_40
            ? {2'b10, glyph_r[11], glyph_row[2:0], glyph_r[7:0]}
            : {1'b0, glyph_r[12], glyph_row, glyph_r[7:0]};
    end

    /* A color with the pixel format's opaque bit is a pixel. Any other
     * is an entry of the palette, in the bits above it. */
    logic [15:0] c_look, pal_px;
    always_comb begin
        case (fs)
            3'd3: c_look = c_fg;
            3'd4: c_look = c_bg;
            default: c_look = c_ul;
        endcase
        pal_ra = c_look[15:6];
        pal_px = pal_half ? pal_rq[31:16] : pal_rq[15:0];
    end
    function automatic logic [15:0] pixel_of(input logic [15:0] color,
                                             input logic [15:0] entry);
        pixel_of = color[5] ? color : entry;
    endfunction

    /* These steps run in the C renderer's order: under DECSCNM every
     * cell's colors swap, and a blinking cell's glyph is drawn in the
     * background color while its blink phase bit is set. Every pixel is
     * set on a row where a strike or an overline is drawn. An underline in
     * the foreground's color is more pixels of the glyph, and one in a
     * color of its own is drawn over the glyph. Under the block cursor the
     * glyph and its lines are drawn in the cell's background color on the
     * cursor color, and the underline cursor is the cursor color all the
     * way across. */
    logic cur_here, cur_block, cur_under;
    logic [7:0] ul_bits, glyph_bits;
    logic [15:0] cell_fg, cell_bg;
    logic [7:0] bits_res, ulb_res;
    logic [15:0] fg_res, bg_res;
    always_comb begin
        /* The cell being resolved is one ahead of the one px is in. */
        cur_here = cur_hit && (wide ? 7'((px + 10'd1) >> 4)
                                    : 7'((px + 10'd1) >> 3)) == cur_cx;
        cur_block = cur_here && (cur_style == 3'd0 || cur_style == 3'd1
                                 || cur_style == 3'd2);
        cur_under = cur_here && (cur_style == 3'd3 || cur_style == 3'd4)
            && (use_40 ? scanrow == 4'd7
                       : scanrow == 4'd14 || scanrow == 4'd15);
        cell_fg = reverse_q ? r_bg : r_fg;
        cell_bg = reverse_q ? r_fg : r_bg;
        ul_bits = underline(use_40, ul_row, attr_r[4:2]);
        glyph_bits = (attr_r[5] && strike_row) || (attr_r[6] && over_row)
            ? 8'hFF : font_r;
        fg_res = (attr_r[1:0] & blink_q) != 2'd0 ? cell_bg : cell_fg;
        bg_res = cell_bg;
        if (attr_r[8]) begin  /* MODE0_ATTR_UL_COLOR */
            bits_res = glyph_bits;
            ulb_res = ul_bits;
        end else begin
            bits_res = glyph_bits | ul_bits;
            ulb_res = 8'h00;
        end
        if (cur_block) begin
            bits_res = glyph_bits | ul_bits;
            ulb_res = 8'h00;
            fg_res = cell_bg;
            bg_res = cursor_color_q;
        end
        if (cur_under) begin
            bits_res = 8'hFF;
            ulb_res = 8'h00;
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
     * row. A column past the row's is the last. */
    logic [7:0] cols_row;
    always_comb cols_row = row_word[14:13] != 2'd0
        ? (use_40 ? 8'd20 : 8'd40) : (use_40 ? 8'd40 : 8'd80);

    /* The cell being written out. */
    logic [7:0] shreg, shul;
    logic [15:0] fg_r, bg_r, ul_r;
    logic take;  // the fetched cell becomes the one written out
    always_comb take = pre ? fs == 3'd7 : phase == phase_last;

    logic [14:0] hw_next;
    always_comb hw_next = hw + 15'd3;

    /* A row in a scroll part way across draws the rows where they were,
     * moved on by the scanlines the scroll has gone, and the row going
     * out at the edge it leaves by, as mode0.c does. The row's line, moved
     * on by sm_off, falls in the row whose place it shows, so the line
     * this row draws is that, back a row on an upward scroll. */
    logic [4:0] lr0, lr2;
    logic [8:0] s2;
    logic in_reg;
    always_comb begin
        lr0 = use_40 ? term_line[7:3] : term_line[8:4];
        s2 = term_line + {4'd0, sm_off};
        lr2 = use_40 ? s2[7:3] : s2[8:4];
        in_reg = sm_act && lr0 >= sm_top_q && lr0 <= sm_bot_q;
    end

    initial begin
        wr_bank = 1'b0;
        run = 1'b0;
        remap = 1'b0;
        setup = 1'b0;
        ghost_r = 1'b0;
        pre = 1'b0;
        term_line = '0;
        size_r = '0;
        cur_hit = 1'b0;
        cur_cx = '0;
        cur_style = '0;
        px = '0;
        fs = '0;
        hw = '0;
        cell_ra = '0;
        col_ra = '0;
        glyph_r = '0;
        attr_r = '0;
        cw0 = '0;
        c_fg = '0;
        c_bg = '0;
        c_ul = '0;
        r_fg = '0;
        r_bg = '0;
        r_ul = '0;
        f_sent = 1'b0;
        f_gnt_d = 1'b0;
        font_r = '0;
        fg_r = '0;
        bg_r = '0;
        ul_r = '0;
        shreg = '0;
        shul = '0;
    end
    always_ff @(posedge clk) begin
        f_gnt_d <= f_gnt;
        if (f_gnt_d)
            font_r <= f_data;
        if (line_start) begin
            if (pair_start)
                wr_bank <= !wr_bank;
        end else if (term_go) begin
            run <= 1'b1;
            remap <= 1'b1;
            setup <= 1'b1;
            pre <= 1'b1;
            px <= '0;
            ghost_r <= 1'b0;
            term_line <= t[8:0] - term_cfg[8:0];
        end else if (run && remap) begin
            if (in_reg) begin
                ghost_r <= sm_up_q ? lr2 == sm_top_q : lr2 == sm_bot_q + 5'd1;
                term_line <= s2 - (sm_up_q ? (use_40 ? 9'd8 : 9'd16) : 9'd0);
            end
            remap <= 1'b0;
        end else if (run && setup) begin
            size_r <= row_word[14:13];
            cur_hit <= !ghost_r && cursor_q[25]  /* enabled */
                && (cursor_q[24]     /* lit, or steady style */
                    || cursor_q[18:16] == 3'd2
                    || cursor_q[18:16] == 3'd4
                    || cursor_q[18:16] == 3'd6)
                && cursor_q[15:8] == {3'd0, logical_row};
            cur_cx <= cursor_q[7:0] >= cols_row
                ? 7'(cols_row - 8'd1) : cursor_q[6:0];
            cur_style <= cursor_q[18:16];
            cell_ra <= row_word[12:0];
            hw <= {1'b0, row_word[12:0], 1'b0} + {2'b00, row_word[12:0]};
            col_ra <= 14'(({1'b0, row_word[12:0], 1'b0}
                           + {2'b00, row_word[12:0]}) >> 1);
            fs <= '0;
            f_sent <= 1'b0;
            setup <= 1'b0;
        end else if (run) begin
            if (fs != 3'd7)
                fs <= fs + 3'd1;
            if (f_gnt)
                f_sent <= 1'b1;
            case (fs)
                3'd0: col_ra <= col_ra + 14'd1;
                3'd1: begin
                    glyph_r <= cell_rq[15:0];
                    attr_r <= cell_rq[31:16];
                    cw0 <= col_rq;
                end
                3'd2: begin
                    if (hw[0]) begin
                        c_fg <= cw0[31:16];
                        c_bg <= col_rq[15:0];
                        c_ul <= col_rq[31:16];
                    end else begin
                        c_fg <= cw0[15:0];
                        c_bg <= cw0[31:16];
                        c_ul <= col_rq[15:0];
                    end
                end
                3'd4: r_fg <= pixel_of(c_fg, pal_px);
                3'd5: r_bg <= pixel_of(c_bg, pal_px);
                3'd6: r_ul <= pixel_of(c_ul, pal_px);
                default: ;
            endcase
            if (!pre) begin
                if (!wide || px[0]) begin
                    shreg <= {shreg[6:0], 1'b0};
                    shul <= {shul[6:0], 1'b0};
                end
                px <= px + 10'd1;
            end
            if (take) begin
`ifdef VERILATOR
                /* The font store answered every other fetch in time. */
                if (!f_sent || f_gnt_d)
                    $fatal(1, "mode0 glyph late");
`endif
                fg_r <= fg_res;
                bg_r <= bg_res;
                ul_r <= r_ul;
                shreg <= bits_res;
                shul <= ulb_res;
                cell_ra <= cell_ra + 13'd1;
                hw <= hw_next;
                col_ra <= hw_next[14:1];
                fs <= '0;
                f_sent <= 1'b0;
                pre <= 1'b0;
                if (!pre && px == (use_40 ? 10'd319 : 10'd639))
                    run <= 1'b0;
            end
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
        if (!line_start && !term_go && run && !setup && !pre)
            linebuf[{wr_bank, px}] <=
                (cur_bar_out && phase < (use_40 ? 4'd1 : 4'd2))
                    ? cursor_color_q
                    : shul[7] ? ul_r : (shreg[7] ? fg_r : bg_r);

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
                                    cursor_q[31:26], cursor_q[23:19], t[9],
                                    glyph_r[15:13], glyph_r[10:8],
                                    attr_r[15:9], attr_r[7], c_look[5:0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
