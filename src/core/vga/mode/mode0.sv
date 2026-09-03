/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The terminal's cell memory and scanout registers. The cells are the
 * shared ANSI engine's screen buffers, placed behind this window by the
 * linker so its ordinary stores land here unchanged.
 *
 * Firmware writes are shadows; the scanout latches them at frame start,
 * for one frame of latency and never a tear.
 *
 * The cells power up zero — BRAM contents ship in the bitstream — so
 * there is no boot-time clear.
 */

module mode0 (
    input logic clk,
    /* The cells run on the clock that does not stop, so the savestate
     * serializer can read them with the render standing still. */
    input logic clk_mem,
    input logic frame_start,

    input logic [9:0] h,
    input logic [9:0] v,
    input logic px_last,
    input logic line_start,
    input logic [9:0] cw,
    output logic [15:0] mode0_pix,

    output logic mode0_f_req,
    output logic [13:0] mode0_f_addr,
    input logic f_gnt,
    input logic [7:0] f_data,

    /* The savestate serializer, which owns the bus side while it has
     * the machine. It reads and writes whole words; the byte strobes
     * belong to the machine's own writes. */
    input logic sst_own,
    input logic [13:0] sst_addr,
    input logic sst_we,
    input logic [31:0] sst_wdata,
    output logic [31:0] mode0_sst_rdata,

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
    always_comb cell_idx = sst_own ? sst_addr : b_addr[15:2];

    /* cursor {enabled[25], lit[24], style[23:16], y[15:8], x[7:0]};
     * prog {enable[31], end[25:16], begin[9:0]}. */
    logic [15:0] row_shadow[32] /*verilator public_flat_rd*/;
    logic [31:0] cursor_shadow /*verilator public_flat_rd*/;
    logic [15:0] cursor_color_shadow /*verilator public_flat_rd*/;
    logic [1:0] blink_shadow /*verilator public_flat_rd*/;
    logic [31:0] prog_shadow /*verilator public_flat_rd*/;
    logic [31:0] frame_count /*verilator public_flat_rd*/;

    /* The scanout read stands alone and unreset: a block RAM's output
     * register has no asynchronous clear, and a read inside the
     * pipeline's reset would keep the cells out of memory entirely. */
    always_ff @(posedge clk_mem)
        fetch_q <= {cell3[fetch_word], cell2[fetch_word],
                    cell1[fetch_word], cell0[fetch_word]};

    logic [31:0] cells_q, regs_q;
    logic sel_cells;
    always_comb mode0_b_rdata = sel_cells ? cells_q : regs_q;

    always_comb mode0_sst_rdata = cells_q;

    logic cell_w0, cell_w1, cell_w2, cell_w3;
    logic [31:0] cell_d;
    always_comb begin
        cell_w0 = sst_own ? sst_we : (b_stb && !b_addr[16] && b_we
                                      && b_wstrb[0]);
        cell_w1 = sst_own ? sst_we : (b_stb && !b_addr[16] && b_we
                                      && b_wstrb[1]);
        cell_w2 = sst_own ? sst_we : (b_stb && !b_addr[16] && b_we
                                      && b_wstrb[2]);
        cell_w3 = sst_own ? sst_we : (b_stb && !b_addr[16] && b_we
                                      && b_wstrb[3]);
        cell_d = sst_own ? sst_wdata : b_wdata;
    end

    always_ff @(posedge clk_mem) begin
        if (sst_own || b_stb) begin
            cells_q <= {cell3[cell_idx], cell2[cell_idx],
                        cell1[cell_idx], cell0[cell_idx]};
            if (cell_w0) cell0[cell_idx] <= cell_d[7:0];
            if (cell_w1) cell1[cell_idx] <= cell_d[15:8];
            if (cell_w2) cell2[cell_idx] <= cell_d[23:16];
            if (cell_w3) cell3[cell_idx] <= cell_d[31:24];
        end
    end
    always_ff @(posedge clk) begin
        if (b_stb) begin
            sel_cells <= !b_addr[16];
            if (!b_addr[16]) begin
            end else begin
                case (b_addr[7:2])
                    6'd32: regs_q <= cursor_shadow;
                    6'd33: regs_q <= {16'd0, cursor_color_shadow};
                    6'd34: regs_q <= {30'd0, blink_shadow};
                    6'd35: regs_q <= prog_shadow;
                    6'd40: regs_q <= frame_count;
                    default: regs_q <= {16'd0, row_shadow[b_addr[6:2]]};
                endcase
            end
        end
    end

    initial begin
        for (int i = 0; i < 32; i++)
            row_shadow[i] = 16'h0000;
        cursor_shadow = 32'h0;
        cursor_color_shadow = 16'h0;
        blink_shadow = 2'h0;
        prog_shadow = 32'h0;
        frame_count = 32'h0;
    end
    always_ff @(posedge clk) begin
        if (frame_start)
            frame_count <= frame_count + 32'd1;
        if (b_stb && b_we && b_addr[16]) begin
            case (b_addr[7:2])
                6'd32: cursor_shadow <= b_wdata;
                6'd33: cursor_color_shadow <= b_wdata[15:0];
                6'd34: blink_shadow <= b_wdata[1:0];
                6'd35: prog_shadow <= b_wdata;
                6'd40: ;  /* the frame counter is the raster's */
                default: begin
                    if (!b_addr[7])
                        row_shadow[b_addr[6:2]] <= b_wdata[15:0];
                end
            endcase
        end
    end

    /* Latched one line before the beam's frame, where the render takes
     * row 0, so a mid-frame publish never tears. */
    logic frame_render;
    always_comb frame_render = line_start && v == 10'd524;
    logic [15:0] row_base[32];
    logic [31:0] cursor_q;
    logic [15:0] cursor_color_q;
    logic [1:0] blink_q;
    logic [31:0] prog_q;
    initial begin
        for (int i = 0; i < 32; i++)
            row_base[i] = 16'h0000;
        cursor_q = 32'h0;
        cursor_color_q = 16'h0;
        blink_q = 2'h0;
        prog_q = 32'h0;
    end
    always_ff @(posedge clk) begin
        if (frame_render) begin
            for (int i = 0; i < 32; i++)
                row_base[i] <= row_shadow[i];
            cursor_q <= cursor_shadow;
            cursor_color_q <= cursor_color_shadow;
            blink_q <= blink_shadow;
            prog_q <= prog_shadow;
        end
    end

    /* Eight clocks a cell against a line of 800; the margin is why the
     * fetches can be serial. The bank rides in the address, because a
     * bank index outside it reads both halves and muxes, which is an
     * asynchronous read and no block RAM at all. */
    (* ramstyle = "no_rw_check" *)
    logic [15:0] linebuf[2048];

    /* Its own unreset block: a port inside the pipeline's reset keeps
     * the whole buffer out of memory. */
    logic lb_we;
    logic lb_bank;
    logic [9:0] lb_addr;
    logic [15:0] lb_data;
    always_ff @(posedge clk)
        if (lb_we)
            linebuf[{lb_bank, lb_addr}] <= lb_data;
    logic wr_bank;
    logic [9:0] t;  // the target line
    logic t_active;
    logic [8:0] term_line;
    logic [4:0] logical_row;
    logic [3:0] scanrow;
    logic [7:0] line_mask;
    logic ul_row;
    logic cur_hit;  // cursor overlay lands on this line
    logic [6:0] cur_cx;
    logic [2:0] cur_style;  // wrap-park forces block

    logic [6:0] rescol;  // the cell being resolved, one ahead of px
    logic [9:0] px;    // write pointer into the line
    logic [3:0] step;
    logic run /*verilator public_flat_rd*/;

    logic [31:0] w0_n, w1_n;
    logic [13:0] fetch_word;
    logic [31:0] fetch_q;
    logic [7:0] bits;
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
    /* The cell's word lands seven clocks before its glyph is needed, so
     * the request stands and the byte is there in time. */
    always_comb begin
        case (font_sel)
            2'd1: mode0_f_addr = use_40
                ? {2'b11, 3'b001, 1'b0, scanrow[2:0], font_code[4:0]}
                : {2'b11, 3'b000, scanrow, font_code[4:0]};
            2'd2: mode0_f_addr = {2'b10, 1'b0, scanrow, font_code[6:0]};
            default: mode0_f_addr = use_40
                ? {2'b01, 1'b0, scanrow[2:0], font_code}
                : {2'b00, scanrow, font_code};
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
     * fewer row bit everywhere and the 8-row attribute lines. */
    logic use_40;
    always_comb use_40 = cw == 10'd320;
    always_comb begin
        if (use_40) begin
            scanrow = {1'b0, term_line[2:0]};
            logical_row = term_line[7:3];
            line_mask = {2'b00,
                         scanrow == 4'd0,                  /* ATTR_OVERLINE */
                         scanrow == 4'd4,                  /* ATTR_STRIKE */
                         scanrow == 4'd7 || scanrow == 4'd5, /* ATTR_DBL_UL */
                         scanrow == 4'd7,                  /* ATTR_UNDERLINE */
                         2'b00};
            ul_row = scanrow == 4'd7 || scanrow == 4'd5;
        end else begin
            scanrow = term_line[3:0];
            logical_row = term_line[8:4];
            line_mask = {2'b00,
                         scanrow == 4'd0,                    /* ATTR_OVERLINE */
                         scanrow == 4'd8,                    /* ATTR_STRIKE */
                         scanrow == 4'd15 || scanrow == 4'd13, /* ATTR_DBL_UL */
                         scanrow == 4'd15,                   /* ATTR_UNDERLINE */
                         2'b00};
            ul_row = scanrow == 4'd15 || scanrow == 4'd13;
        end
    end

    /* The C renderer's exact order: blink darkens unless the block
     * cursor owns the cell, line rows force the stroke and take the
     * underline colour, then the block cursor swaps in its own ground
     * with the cell's background as ink. */
    logic cur_here, cur_block;
    logic [7:0] attr_r;
    logic [7:0] bits_res;
    logic [15:0] fg_res, bg_res;
    always_comb begin
        attr_r = w0_n[15:8];
        cur_here = cur_hit && rescol == cur_cx;
        cur_block = cur_here && (cur_style == 3'd0 || cur_style == 3'd1
                                 || cur_style == 3'd2);
        bits_res = font_bits;
        fg_res = w0_n[31:16];
        bg_res = w1_n[15:0];
        if (!cur_block && (attr_r & {6'b0, blink_q}) != 8'h00)
            fg_res = w1_n[15:0];
        if ((attr_r & line_mask) != 8'h00) begin
            bits_res = 8'hFF;
            if (ul_row && !cur_block)
                fg_res = w1_n[31:16];
        end
        if (cur_block) begin
            fg_res = w1_n[15:0];
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

    logic cur_bar;
    always_comb cur_bar = cur_here && (cur_style == 3'd5 || cur_style == 3'd6);

    initial begin
        wr_bank = 1'b0;
        run = 1'b0;
        t = '0;
        t_active = 1'b0;
        term_line = '0;
        cur_hit = 1'b0;
        cur_cx = '0;
        cur_style = '0;
        rescol = '0;
        px = '0;
        step = '0;
        w0_n = '0;
        w1_n = '0;
        fetch_word = '0;
        bits = '0;
        fg_r = '0;
        bg_r = '0;
        shreg = '0;
    end
    always_ff @(posedge clk) begin
        lb_we <= 1'b0;
        if (line_start) begin
            wr_bank <= !wr_bank;
            t <= v == 10'd524 ? 10'd0 : v + 10'd1;
            run <= 1'b1;
            rescol <= '0;
            px <= '0;
            step <= '0;
        end else if (run && step == 4'd0) begin
            t_active <= prog_q[31] && t >= prog_q[9:0]
                && t < prog_q[25:16];
            term_line <= t[8:0] - prog_q[8:0];
            step <= 4'd1;
        end else if (run) begin
            case (step)
                4'd1: begin
                    cur_hit <= cursor_q[25]  /* enabled */
                        && (cursor_q[24]     /* lit, or steady style */
                            || cursor_q[18:16] == 3'd2
                            || cursor_q[18:16] == 3'd4
                            || cursor_q[18:16] == 3'd6)
                        && cursor_q[15:8] == {3'd0, logical_row};
                    cur_cx <= cursor_q[7:0] >= (use_40 ? 8'd40 : 8'd80)
                        ? (use_40 ? 7'd39 : 7'd79) : cursor_q[6:0];
                    cur_style <= cursor_q[7:0] >= (use_40 ? 8'd40 : 8'd80)
                        ? 3'd1 : cursor_q[18:16];
                    fetch_word <= row_base[logical_row][15:2];
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
                    w1_n <= fetch_q;
                    fetch_word <= fetch_word + 14'd1;
                    step <= 4'd5;
                end
                4'd5: begin
                    bits <= bits_res;
                    fg_r <= fg_res;
                    bg_r <= bg_res;
                    shreg <= bits_res;
                    rescol <= 7'd1;
                    fetch_word <= fetch_word + 14'd1;
                    step <= 4'd6;
                end
                default: begin
                    lb_we <= 1'b1;
                    lb_bank <= wr_bank;
                    lb_addr <= px;
                    lb_data <= t_active
                        ? ((cur_bar_out && px[2:0] < (use_40 ? 3'd1 : 3'd2))
                               ? cursor_color_q
                               : (shreg[7] ? fg_r : bg_r))
                        : 16'h0000;
                    shreg <= {shreg[6:0], 1'b0};
                    px <= px + 10'd1;
                    case (px[2:0])
                        3'd0: w0_n <= fetch_q;
                        3'd1: w1_n <= fetch_q;
                        3'd6: fetch_word <= fetch_word + 14'd1;
                        3'd7: begin
                            bits <= bits_res;
                            fg_r <= fg_res;
                            bg_r <= bg_res;
                            shreg <= bits_res;
                            rescol <= rescol + 7'd1;
                            fetch_word <= fetch_word + 14'd1;
                            if (px == (use_40 ? 10'd319 : 10'd639))
                                run <= 1'b0;
                        end
                        default: ;
                    endcase
                end
            endcase
        end
    end

    /* The bar cursor patches the cell being written out, which is one
     * behind the cell being resolved. */
    logic cur_bar_out;
    always_comb cur_bar_out = cur_hit && t_active
        && px[9:3] == cur_cx
        && (cur_style == 3'd5 || cur_style == 3'd6);

    /* The bank toggle lands on h==0's first tick, so only the pixel-0
     * read at the end of h==799 still sees the line under its write-side
     * label. */
    logic [10:0] lb_rd;
    always_comb lb_rd = h == 10'd799
        ? {wr_bank, 10'd0}
        : {!wr_bank, 10'(h + 10'd1)};

    /* The buffer's output register carries nothing but the buffer: a
     * branch handing it a constant makes the fabric read combinationally
     * and mux, and the line buffer leaves memory. */
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
    always_comb unused_mode0 = ^{b_addr[1:0], bits, cur_bar,
                                    prog_q[30:26], prog_q[15:10],
                                    cursor_q[31:26], cursor_q[23:19], t[9]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
