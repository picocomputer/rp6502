/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The terminal's cell memory and scanout registers. The cells are the four
 * screen buffers of vga/term/term.c — 61,440 bytes the linker places behind
 * the soft CPU's 0x5 window, so the shared ANSI engine's ordinary stores
 * land here unchanged. The register bank above them carries what the
 * renderer needs each frame: the visible rows' base offsets (term.c owns
 * the O(1) scroll remap and the firmware publishes the resolved bases once
 * per frame), the cursor, the blink phase, and the prog window.
 *
 * Registers written by the firmware are shadows; the scanout latches them
 * at frame start, one frame of latency and never a tear. The scanout side
 * arrives with vid_compose; until then the shadows and the frame counter
 * are the whole story.
 *
 * The cells power up zero — BRAM contents ship in the bitstream, so there
 * is no boot-time clear; term.c's own lazy row clears do the rest.
 */

module vid_term (
    input logic clk,
    input logic rst_n,
    input logic frame_start,

    /* The raster: the renderer works one line ahead of the beam, into the
     * ping-pong line buffer the beam reads behind it. */
    input logic [9:0] h,
    input logic [9:0] v,
    input logic px_last,
    input logic line_start,
    output logic [15:0] vid_term_pix,

    /* The soft CPU's window: bit 16 low is cell memory, word-wide with
     * byte lanes; bit 16 high is the register bank. */
    input logic b_stb,
    input logic b_we,
    input logic [16:0] b_addr,
    input logic [3:0] b_wstrb,
    input logic [31:0] b_wdata,
    output logic [31:0] vid_term_b_rdata
);

    logic [31:0] cells[15360] /*verilator public_flat_rw*/;

    logic [13:0] cell_idx;
    always_comb cell_idx = b_addr[15:2];

    /* Row bases as byte offsets into the cell window, cursor packed
     * {enabled[25], lit[24], style[23:16], y[15:8], x[7:0]}, the cursor
     * color, the blink phase, and prog {enable[31], end[25:16], begin[9:0]}. */
    logic [15:0] row_shadow[32] /*verilator public_flat_rd*/;
    logic [31:0] cursor_shadow /*verilator public_flat_rd*/;
    logic [15:0] cursor_color_shadow /*verilator public_flat_rd*/;
    logic [1:0] blink_shadow /*verilator public_flat_rd*/;
    logic [31:0] prog_shadow /*verilator public_flat_rd*/;
    logic [31:0] frame_count /*verilator public_flat_rd*/;

    always_ff @(posedge clk) begin
        if (b_stb) begin
            if (!b_addr[16]) begin
                vid_term_b_rdata <= cells[cell_idx];
                if (b_we) begin
                    if (b_wstrb[0])
                        cells[cell_idx][7:0] <= b_wdata[7:0];
                    if (b_wstrb[1])
                        cells[cell_idx][15:8] <= b_wdata[15:8];
                    if (b_wstrb[2])
                        cells[cell_idx][23:16] <= b_wdata[23:16];
                    if (b_wstrb[3])
                        cells[cell_idx][31:24] <= b_wdata[31:24];
                end
            end else begin
                case (b_addr[7:2])
                    6'd32: vid_term_b_rdata <= cursor_shadow;
                    6'd33: vid_term_b_rdata <= {16'd0, cursor_color_shadow};
                    6'd34: vid_term_b_rdata <= {30'd0, blink_shadow};
                    6'd35: vid_term_b_rdata <= prog_shadow;
                    6'd40: vid_term_b_rdata <= frame_count;
                    default: vid_term_b_rdata <=
                        {16'd0, row_shadow[b_addr[6:2]]};
                endcase
            end
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int i = 0; i < 32; i++)
                row_shadow[i] <= 16'h0000;
            cursor_shadow <= 32'h0;
            cursor_color_shadow <= 16'h0;
            blink_shadow <= 2'h0;
            prog_shadow <= 32'h0;
            frame_count <= 32'h0;
        end else begin
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
    end

    /* Scanout state, latched at frame start so a mid-frame publish never
     * tears. */
    logic [15:0] row_base[32];
    logic [31:0] cursor_q;
    logic [15:0] cursor_color_q;
    logic [1:0] blink_q;
    logic [31:0] prog_q;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int i = 0; i < 32; i++)
                row_base[i] <= 16'h0000;
            cursor_q <= 32'h0;
            cursor_color_q <= 16'h0;
            blink_q <= 2'h0;
            prog_q <= 32'h0;
        end else if (frame_start) begin
            for (int i = 0; i < 32; i++)
                row_base[i] <= row_shadow[i];
            cursor_q <= cursor_shadow;
            cursor_color_q <= cursor_color_shadow;
            blink_q <= blink_shadow;
            prog_q <= prog_shadow;
        end
    end

    /* The renderer, mirroring term_render_640: during line v it draws the
     * beam's next line into the write bank. Steady state is eight clocks a
     * cell — the two cell words and the font byte fetch under the previous
     * cell's pixel writes — well inside the 800-clock line. */
    logic [15:0] linebuf[2][640];
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
    logic run;

    /* Cell words: {fg, attr, code} and {ul, bg} of the cell being
     * resolved while its predecessor shifts out. */
    logic [31:0] w0_n, w1_n;
    logic [13:0] fetch_word;
    logic [31:0] fetch_q;
    logic [7:0] bits;
    logic [15:0] fg_r, bg_r;
    logic [7:0] shreg;

    /* The font fetch for the cell being resolved. */
    logic [1:0] font_sel;
    logic [7:0] font_code;
    logic [7:0] font_bits;
    always_comb begin
        if (w0_n[14])  /* ATTR_DEC */
            font_sel = 2'd1;
        else if (w0_n[15] && !w0_n[7])  /* ATTR_ITALIC, low half only */
            font_sel = 2'd2;
        else
            font_sel = 2'd0;
        font_code = font_sel == 2'd1 ? w0_n[7:0] - 8'h5F : w0_n[7:0];
    end
    vid_font vid_font (
        .clk(clk),
        .sel(font_sel),
        .code(font_code),
        .row(scanrow),
        .vid_font_bits(font_bits)
    );

    always_comb begin
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

    /* One cell's colors, the C renderer's exact order: blink darkens
     * unless the block cursor owns the cell, line rows force the stroke
     * and take the underline color, the block cursor then swaps in its
     * own ground with the cell's background as ink. */
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
            && (scanrow == 4'd14 || scanrow == 4'd15)) begin
            bits_res = 8'hFF;
            fg_res = cursor_color_q;
            bg_res = cursor_color_q;
        end
    end

    logic cur_bar;
    always_comb cur_bar = cur_here && (cur_style == 3'd5 || cur_style == 3'd6);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wr_bank <= 1'b0;
            run <= 1'b0;
            t <= '0;
            t_active <= 1'b0;
            term_line <= '0;
            cur_hit <= 1'b0;
            cur_cx <= '0;
            cur_style <= '0;
            rescol <= '0;
            px <= '0;
            step <= '0;
            w0_n <= '0;
            w1_n <= '0;
            fetch_word <= '0;
            bits <= '0;
            fg_r <= '0;
            bg_r <= '0;
            shreg <= '0;
        end else begin
            fetch_q <= cells[fetch_word];
            if (line_start) begin
                wr_bank <= !wr_bank;
                t <= v == 10'd524 ? 10'd0 : v + 10'd1;
                run <= 1'b1;
                rescol <= '0;
                px <= '0;
                step <= '0;
            end else if (run && step == 4'd0) begin
                /* Line setup, one clock after line_start so t is held. */
                t_active <= prog_q[31] && t >= prog_q[9:0]
                    && t < prog_q[25:16];
                term_line <= t[8:0] - prog_q[8:0];
                step <= 4'd1;
            end else if (run) begin
                case (step)
                    /* Prologue: fetch and resolve cell 0, then cell 1's
                     * words, before the pixel loop begins. */
                    4'd1: begin
                        cur_hit <= cursor_q[25]  /* enabled */
                            && (cursor_q[24]     /* lit, or steady style */
                                || cursor_q[18:16] == 3'd2
                                || cursor_q[18:16] == 3'd4
                                || cursor_q[18:16] == 3'd6)
                            && cursor_q[15:8] == {3'd0, logical_row};
                        cur_cx <= cursor_q[7:0] >= 8'd80
                            ? 7'd79 : cursor_q[6:0];
                        cur_style <= cursor_q[7:0] >= 8'd80
                            ? 3'd1 : cursor_q[18:16];
                        fetch_word <= row_base[logical_row][15:2];
                        step <= 4'd2;
                    end
                    4'd2: begin
                        fetch_word <= fetch_word + 14'd1;
                        step <= 4'd3;
                    end
                    4'd3: begin
                        /* An address lands one clock after it is set and
                         * captures one after that; cell 0's first word is
                         * on fetch_q now. */
                        w0_n <= fetch_q;
                        step <= 4'd4;
                    end
                    4'd4: begin
                        w1_n <= fetch_q;
                        fetch_word <= fetch_word + 14'd1;
                        step <= 4'd5;
                    end
                    4'd5: begin
                        /* font_bits now carries cell 0; resolve it. */
                        bits <= bits_res;
                        fg_r <= fg_res;
                        bg_r <= bg_res;
                        shreg <= bits_res;
                        rescol <= 7'd1;
                        fetch_word <= fetch_word + 14'd1;
                        step <= 4'd6;
                    end
                    /* Steady state: eight pixel writes for this cell while
                     * the next cell's words and font byte arrive. */
                    default: begin
                        if (t_active)
                            linebuf[wr_bank][px] <=
                                (cur_bar_out && px[2:0] < 3'd2)
                                    ? cursor_color_q
                                    : (shreg[7] ? fg_r : bg_r);
                        else
                            linebuf[wr_bank][px] <= 16'h0000;
                        shreg <= {shreg[6:0], 1'b0};
                        px <= px + 10'd1;
                        case (px[2:0])
                            3'd0: w0_n <= fetch_q;
                            3'd1: w1_n <= fetch_q;
                            3'd6: fetch_word <= fetch_word + 14'd1;
                            3'd7: begin
                                /* Load the resolved next cell. */
                                bits <= bits_res;
                                fg_r <= fg_res;
                                bg_r <= bg_res;
                                shreg <= bits_res;
                                rescol <= rescol + 7'd1;
                                fetch_word <= fetch_word + 14'd1;
                                if (px == 10'd639)
                                    run <= 1'b0;
                            end
                            default: ;
                        endcase
                    end
                endcase
            end
        end
    end

    /* The bar cursor patches the cell being written out, which is one
     * behind the cell being resolved. */
    logic cur_bar_out;
    always_comb cur_bar_out = cur_hit && t_active
        && px[9:3] == cur_cx
        && (cur_style == 3'd5 || cur_style == 3'd6);

    /* The beam reads one position ahead of itself, on each pixel's last
     * tick. The bank toggle lands on h==0's first tick, so only the
     * pixel-0 read at the end of h==799 still sees the line under its
     * write-side label. */
    always_ff @(posedge clk) begin
        if (px_last) begin
            if (h == 10'd799)
                vid_term_pix <= linebuf[wr_bank][10'd0];
            else if (h < 10'd639)
                vid_term_pix <= linebuf[!wr_bank][h + 10'd1];
            else
                vid_term_pix <= 16'h0000;
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_term;
    always_comb unused_vid_term = ^{b_addr[1:0], bits, cur_bar,
                                    prog_q[30:26], prog_q[15:10],
                                    cursor_q[31:26], cursor_q[23:19], t[9]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
