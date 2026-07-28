/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 1, the character cells of vga/modes/mode1.c: a grid of glyph cells
 * — bare glyphs over the two-color palette, packed or reversed nibble
 * colors, byte-indexed colors, or raw sixteen-bit colors with a reserved
 * attribute byte — drawn through an 8x8 or 8x16 font from XRAM or the
 * builtin ROM. Rows map with true wraparound and the oracle's rejects;
 * the palette snapshots per line like mode 3. The next cell's bytes and
 * font line fetch under the current cell's eight pixels, so the emitter
 * only stalls priming, wrapping, or losing the port to another plane —
 * and only the beam's deadline matters.
 */

module vid_mode1
    import vid_palette_pkg::*, vid_font_pkg::*;
(
    input logic clk,
    input logic rst_n,

    input logic start,
    input logic abort_i,
    input logic [15:0] attr,
    input logic [127:0] cfgw,
    input logic [8:0] t_row,
    input logic [9:0] cw,

    output logic vid_mode1_a_req,
    output logic [13:0] vid_mode1_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    output logic vid_mode1_px_we,
    output logic [9:0] vid_mode1_px_addr,
    output logic [15:0] vid_mode1_px_data,

    output logic vid_mode1_done,
    output logic vid_mode1_filled
);

    logic cf_x_wrap, cf_y_wrap;
    logic signed [15:0] cf_x_pos, cf_y_pos, cf_wchars, cf_hchars;
    logic [15:0] cf_data, cf_palette, cf_font;
    always_comb begin
        cf_x_wrap = cfgw[7:0] != 8'h00;
        cf_y_wrap = cfgw[15:8] != 8'h00;
        cf_x_pos = cfgw[31:16];
        cf_y_pos = cfgw[47:32];
        cf_wchars = cfgw[63:48];
        cf_hchars = cfgw[79:64];
        cf_data = cfgw[95:80];
        cf_palette = cfgw[111:96];
        cf_font = cfgw[127:112];
    end

    /* attr[3] picks the 8x16 font; attr[2:0] the cell format. */
    logic fh16;
    logic [2:0] fmt;
    logic [2:0] cell_size;
    logic [3:0] pal_bpp;  // palette depth; 0 = raw colors
    always_comb begin
        fh16 = attr[3];
        fmt = attr[2:0];
        case (fmt)
            3'd0: begin cell_size = 3'd1; pal_bpp = 4'd1; end
            3'd1: begin cell_size = 3'd2; pal_bpp = 4'd4; end
            3'd2: begin cell_size = 3'd2; pal_bpp = 4'd4; end
            3'd3: begin cell_size = 3'd3; pal_bpp = 4'd8; end
            default: begin cell_size = 3'd6; pal_bpp = 4'd0; end
        endcase
    end
    /* The oracle computes these in int16, overflow and all. */
    logic [15:0] width_px;
    always_comb width_px = 16'(cf_wchars) << 3;

    typedef enum logic [2:0] {
        S1_IDLE, S1_WRAP, S1_ADDR, S1_PAL, S1_RUN, S1_BLANK
    } state_t;
    state_t state;

    logic signed [16:0] row;
    logic [3:0] scanrow;
    logic [19:0] sizeof_row;
    logic [19:0] row_off;
    logic [16:0] row_base;
    logic signed [16:0] col;
    logic [9:0] px /*verilator public_flat_rd*/;

    /* int16 like the oracle: ±32768 wraps before the fold sees it. */
    logic [15:0] row16, col16;
    always_comb row16 = {7'd0, t_row} - 16'(cf_y_pos);
    always_comb col16 = 16'd0 - 16'(cf_x_pos);

    /* Read where it is used, so it wants LUT RAM: a block RAM
     * cannot answer without a clock and a register file this
     * wide does not fit. */
    /* LUT RAM carries one write and one read, and this engine reads a
     * foreground and a background entry at once — so the palette is
     * four arrays: the parity split that gives each a single write,
     * doubled so each reader has its own port. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_fg_even[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_fg_odd[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_bg_even[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_bg_odd[128];
    logic pal_xram;
    logic [8:0] pal_n;
    logic [8:0] pal_words;
    always_comb pal_words = 9'd1 << (pal_bpp - 4'd1);
    /* A halfword-aligned palette straddles one more word, entry 0 in the
     * first word's high half. */
    logic [8:0] pal_fetch;
    always_comb pal_fetch = pal_words + {8'd0, cf_palette[1]};
    logic [7:0] pal_w;

    logic font_xram;
    always_comb font_xram = {1'b0, cf_font}
        <= 17'h10000 - (fh16 ? 17'd4096 : 17'd2048);

    /* The cell prefetcher: while the current cell's eight pixels emit,
     * the next cell's bytes (up to three words) and its XRAM font byte
     * gather here. */
    typedef enum logic [2:0] {
        F_IDLE, F_W, F_FONT, F_READY
    } fstate_t;
    fstate_t fstate;
    logic [16:0] cell_addr;   // byte address of the cell being fetched
    logic [1:0] fw_i, fw_c, fw_n;  // word issue/capture counts
    logic [95:0] gather;           // up to three words, lane-aligned below
    logic [1:0] cell_lane;         // the cell's byte offset in its word
    logic gnt_d;

    /* Resolved cell waiting, and the cell being emitted. */
    logic nxt_v;
    logic [7:0] nxt_bits;
    logic [15:0] nxt_fg, nxt_bg;
    logic [7:0] cur_bits;
    logic [15:0] cur_fg, cur_bg;
    logic cur_v;

    /* Byte views of the gathered cell, shifted to its lane. */
    logic [47:0] gview;
    always_comb gview = 48'(gather >> {cell_lane, 3'b000});
    logic [7:0] g_glyph, g_b1, g_b2;
    logic [15:0] g_fg16, g_bg16;
    always_comb begin
        g_glyph = gview[7:0];
        g_b1 = gview[15:8];
        g_b2 = gview[23:16];
        g_fg16 = gview[31:16];
        g_bg16 = gview[47:32];
    end

    /* The load's write port, outside the pipeline's reset so it can be
     * memory at all; both readers' copies take the same words. */
    logic pal_ld;
    always_comb pal_ld = !abort_i && !start && state == S1_PAL
        && pal_xram && pal_bpp != 4'd0 && gnt_d;
    logic pal_we_e, pal_we_o;
    logic [6:0] pal_wa_o;
    logic [15:0] pal_wd_e, pal_wd_o;
    always_comb begin
        pal_we_e = pal_ld && (!cf_palette[1] || {1'b0, pal_w} != pal_words);
        pal_we_o = pal_ld && (!cf_palette[1] || pal_w != 8'd0);
        pal_wa_o = cf_palette[1] ? 7'(pal_w - 8'd1) : pal_w[6:0];
        pal_wd_e = cf_palette[1] ? a_rdata[31:16] : a_rdata[15:0];
        pal_wd_o = cf_palette[1] ? a_rdata[15:0] : a_rdata[31:16];
    end
    always_ff @(posedge clk) begin
        if (pal_we_e) begin
            pal_fg_even[pal_w[6:0]] <= pal_wd_e;
            pal_bg_even[pal_w[6:0]] <= pal_wd_e;
        end
        if (pal_we_o) begin
            pal_fg_odd[pal_wa_o] <= pal_wd_o;
            pal_bg_odd[pal_wa_o] <= pal_wd_o;
        end
    end

    logic [15:0] pal_fg, pal_bg;
    logic [7:0] fg_idx, bg_idx;
    always_comb begin
        case (fmt)
            3'd0: begin fg_idx = 8'd1; bg_idx = 8'd0; end
            3'd1: begin fg_idx = {4'd0, g_b1[7:4]}; bg_idx = {4'd0, g_b1[3:0]}; end
            3'd2: begin fg_idx = {4'd0, g_b1[3:0]}; bg_idx = {4'd0, g_b1[7:4]}; end
            default: begin fg_idx = g_b1; bg_idx = g_b2; end
        endcase
        if (pal_xram) begin
            pal_fg = fg_idx[0] ? pal_fg_odd[fg_idx[7:1]]
                               : pal_fg_even[fg_idx[7:1]];
            pal_bg = bg_idx[0] ? pal_bg_odd[bg_idx[7:1]]
                               : pal_bg_even[bg_idx[7:1]];
        end else if (pal_bpp == 4'd1) begin
            pal_fg = VID_COLOR_2[fg_idx[0]];
            pal_bg = VID_COLOR_2[bg_idx[0]];
        end else begin
            pal_fg = VID_COLOR_256[fg_idx];
            pal_bg = VID_COLOR_256[bg_idx];
        end
    end

    /* The font byte: gathered from XRAM, or the builtin ROM by glyph. */
    logic [7:0] font_gather;
    logic [7:0] builtin_bits;
    always_comb builtin_bits = fh16
        ? VID_FONT16[{g_glyph, scanrow}]
        : VID_FONT8[{g_glyph, scanrow[2:0]}];

    logic signed [17:0] win_w;
    always_comb win_w = $signed({{2{width_px[15]}}, width_px});
    logic in_window;
    always_comb in_window = col >= 0 && 18'(col) < win_w;

    always_comb begin
        vid_mode1_a_req = 1'b0;
        vid_mode1_a_addr = cell_addr[15:2] + {12'd0, fw_i};
        case (state)
            S1_PAL: begin
                vid_mode1_a_req = pal_xram && pal_bpp != 4'd0
                    && pal_n < pal_fetch;
                vid_mode1_a_addr = cf_palette[15:2] + {5'd0, pal_n};
            end
            S1_RUN: begin
                if (fstate == F_W) begin
                    vid_mode1_a_req = fw_i < fw_n;
                    vid_mode1_a_addr = cell_addr[15:2] + {12'd0, fw_i};
                end else if (fstate == F_FONT) begin
                    vid_mode1_a_req = fw_i == 2'd0;
                    vid_mode1_a_addr = font_line_addr[15:2];
                end
            end
            default: ;
        endcase
    end
    logic [16:0] font_line_addr;
    always_comb font_line_addr = {1'b0, cf_font}
        + {5'd0, scanrow, 8'd0} + {9'd0, g_glyph};

    /* The pixel port: emitting, padding outside the window, blanking. */
    logic emit_now;
    always_comb emit_now = state == S1_RUN && in_window && cur_v;
    always_comb begin
        vid_mode1_px_we = 1'b0;
        vid_mode1_px_addr = px;
        vid_mode1_px_data = 16'h0000;
        if (state == S1_RUN && !in_window)
            vid_mode1_px_we = 1'b1;
        else if (emit_now) begin
            vid_mode1_px_we = 1'b1;
            vid_mode1_px_data = cur_bits[3'd7 - col[2:0]] ? cur_fg : cur_bg;
        end else if (state == S1_BLANK)
            vid_mode1_px_we = 1'b1;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S1_IDLE;
            fstate <= F_IDLE;
            row <= '0;
            scanrow <= '0;
            sizeof_row <= '0;
            row_off <= '0;
            row_base <= '0;
            col <= '0;
            px <= '0;
            pal_xram <= 1'b0;
            pal_n <= '0;
            pal_w <= '0;
            cell_addr <= '0;
            cell_lane <= '0;
            fetch_col <= '0;
            fw_i <= '0;
            fw_c <= '0;
            fw_n <= '0;
            gather <= '0;
            gnt_d <= 1'b0;
            nxt_v <= 1'b0;
            nxt_bits <= '0;
            nxt_fg <= '0;
            nxt_bg <= '0;
            cur_v <= 1'b0;
            cur_bits <= '0;
            cur_fg <= '0;
            cur_bg <= '0;
            font_gather <= '0;
            vid_mode1_done <= 1'b0;
            vid_mode1_filled <= 1'b0;
        end else begin
            gnt_d <= a_gnt;
            vid_mode1_done <= 1'b0;
            if (abort_i) begin
                if (state != S1_IDLE)
                    $fatal(1, "vid_mode1 underrun px=%0d col=%0d st=%0d f=%0d nxt=%0d cur=%0d fc=%0d", px, col, state, fstate, nxt_v, cur_v, fetch_col);
            end else if (start) begin
                row <= $signed({row16[15], row16});
                sizeof_row <= 20'(17'(cf_wchars[15:0])
                                  * {14'd0, cell_size});
                col <= $signed({col16[15], col16});
                px <= '0;
                cur_v <= 1'b0;
                nxt_v <= 1'b0;
                fstate <= F_IDLE;
                state <= S1_WRAP;
                fetch_col <= '0;
            end else begin
                case (state)
                    S1_IDLE: ;
                    S1_WRAP: begin
                        /* The oracle rejects on the int16 height, not the
                         * char count. */
                        if (cf_wchars < 16'sd1 || height_px_s < 18'sd1)
                            state <= S1_BLANK;
                        else if (cf_y_wrap && row < 0)
                            row <= 17'(18'(row) + height_px_s);
                        else if (cf_y_wrap && 18'(row) >= height_px_s)
                            row <= 17'(18'(row) - height_px_s);
                        else if (cf_x_wrap && col < 0)
                            col <= 17'(18'(col) + win_w);
                        else if (cf_x_wrap && 18'(col) >= win_w)
                            col <= 17'(18'(col) - win_w);
                        else if (row < 0 || 18'(row) >= height_px_s)
                            state <= S1_BLANK;
                        else begin
                            row_off <= 20'((37'(row[15:0])
                                            >> (fh16 ? 4 : 3))
                                           * 37'(sizeof_row));
                            scanrow <= fh16 ? row[3:0] : {1'b0, row[2:0]};
                            state <= S1_ADDR;
                        end
                    end
                    S1_ADDR: begin
                        row_base <= {1'b0, cf_data} + row_off[16:0];
                        if (35'(cf_hchars[14:0]) * 35'(sizeof_row)
                            > 35'(17'h10000) - 35'({1'b0, cf_data}))
                            state <= S1_BLANK;
                        else begin
                            pal_xram <= pal_bpp != 4'd0 && !cf_palette[0]
                                && {1'b0, cf_palette}
                                    <= 17'h10000
                                        - (17'd2 << {13'd0, pal_bpp});
                            pal_n <= '0;
                            pal_w <= '0;
                            state <= S1_PAL;
                        end
                    end
                    S1_PAL: begin
                        if (!pal_xram || pal_bpp == 4'd0) begin
                            state <= S1_RUN;
                            fstate <= F_IDLE;
                            fetch_col <= col < 0 ? 16'd0 : col[15:0] >> 3;
                        end else begin
                            if (a_gnt)
                                pal_n <= pal_n + 9'd1;
                            if (gnt_d) begin
                                pal_w <= pal_w + 8'd1;
                                if ({1'b0, pal_w} == pal_fetch - 9'd1)
                                begin
                                    state <= S1_RUN;
                                    fstate <= F_IDLE;
                                    fetch_col <= col < 0
                                        ? 16'd0 : col[15:0] >> 3;
                                    pal_w <= '0;
                                end
                            end
                        end
                    end
                    S1_RUN: begin
                        /* The fetcher gathers the next cell. */
                        case (fstate)
                            F_IDLE: begin
                                if (!nxt_v
                                    && $signed(18'(fetch_col) <<< 3)
                                       < win_w) begin
                                    cell_addr <= cell_fetch_addr;
                                    cell_lane <= cell_fetch_addr[1:0];
                                    fw_i <= '0;
                                    fw_c <= '0;
                                    fw_n <= 2'((4'(cell_fetch_addr[1:0])
                                         + {1'd0, cell_size} + 4'd3) >> 2);
                                    gather <= '0;
                                    fstate <= F_W;
                                end
                            end
                            F_W: begin
                                if (a_gnt)
                                    fw_i <= fw_i + 2'd1;
                                if (gnt_d) begin
                                    gather <= gather
                                        | (96'(a_rdata) << {fw_c, 5'd0});
                                    fw_c <= fw_c + 2'd1;
                                    if (fw_c + 2'd1 == fw_n) begin
                                        fw_i <= '0;
                                        fstate <= font_xram
                                            ? F_FONT : F_READY;
                                    end
                                end
                            end
                            F_FONT: begin
                                if (a_gnt)
                                    fw_i <= 2'd1;
                                if (gnt_d) begin
                                    font_gather <= a_rdata[
                                        {font_line_byte, 3'b000}+:8];
                                    fstate <= F_READY;
                                end
                            end
                            F_READY: begin
                                if (!nxt_v) begin
                                    nxt_bits <= font_xram
                                        ? font_gather : builtin_bits;
                                    nxt_fg <= fmt == 3'd4 ? g_fg16 : pal_fg;
                                    nxt_bg <= fmt == 3'd4 ? g_bg16 : pal_bg;
                                    nxt_v <= 1'b1;
                                    fetch_col <= fetch_col + 16'd1;
                                    fstate <= F_IDLE;
                                end
                            end
                            default: fstate <= F_IDLE;
                        endcase

                        /* The emitter. */
                        if (!in_window) begin
                            px <= px + 10'd1;
                            col <= col + 17'sd1;
                            if (col + 17'sd1 == 0) begin
                                /* Entering the window: the lead-in's
                                 * prefetch targeted cell 0. */
                                cur_v <= 1'b0;
                            end
                            if (px == cw - 10'd1)
                                line_end(1'b1);
                        end else if (!cur_v) begin
                            /* Load the emitting cell from the prefetch. */
                            if (nxt_v) begin
                                cur_bits <= nxt_bits;
                                cur_fg <= nxt_fg;
                                cur_bg <= nxt_bg;
                                cur_v <= 1'b1;
                                nxt_v <= 1'b0;
                            end
                        end else begin
                            px <= px + 10'd1;
                            if (cf_x_wrap && 18'(col) == win_w - 18'sd1)
                            begin
                                col <= '0;
                                cur_v <= 1'b0;
                                nxt_v <= 1'b0;
                                fetch_col <= '0;
                                fstate <= F_IDLE;
                            end else begin
                                col <= col + 17'sd1;
                                if (col[2:0] == 3'd7) begin
                                    cur_v <= 1'b0;  /* next cell loads */
                                end
                            end
                            if (px == cw - 10'd1)
                                line_end(1'b1);
                        end
                    end
                    S1_BLANK: begin
                        px <= px + 10'd1;
                        if (px == cw - 10'd1)
                            line_end(1'b0);
                    end
                    default: state <= S1_IDLE;
                endcase
            end
        end
    end

    logic [15:0] height_px;
    always_comb height_px = 16'(cf_hchars) << (fh16 ? 4'd4 : 4'd3);
    logic signed [17:0] height_px_s;
    always_comb height_px_s = $signed({{2{height_px[15]}}, height_px});

    /* The next cell the fetcher gathers, advanced on every handoff. */
    logic [15:0] fetch_col;
    logic [16:0] cell_fetch_addr;
    always_comb cell_fetch_addr = row_base
        + 17'(fetch_col * 16'({13'd0, cell_size}));
    logic [1:0] font_line_byte;
    always_comb font_line_byte = font_line_addr[1:0];

    task automatic line_end(input logic filled);
        state <= S1_IDLE;
        vid_mode1_done <= 1'b1;
        vid_mode1_filled <= filled;
    endtask

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_mode1;
    always_comb unused_vid_mode1 = ^{cfgw, attr[15:4], sizeof_row,
                                     row_off[19:17], gather,
                                     win_w[17], cell_addr[16],
                                     cell_addr[1:0],
                                     font_line_addr[16]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
