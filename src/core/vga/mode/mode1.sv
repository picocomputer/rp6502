/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 1, the character cells of core/vga/mode/mode1.c: rows mapped with
 * true wraparound, the oracle's rejects, the cell gather, the font fetch
 * from XRAM or the builtin store, and each cell's two colors resolved
 * through the plane's palette. A font row is a 1bpp bitmap and a cell's
 * fg/bg is a two-entry palette, so every cell reaches the shared pixel
 * tail as one immediate segment.
 *
 * The one front that keeps the palette store, because it resolves colors
 * before the tail sees them; the tail's own palette machinery idles.
 */

module mode1 (
    input logic clk,

    input logic start,
    input logic abort_i,
    input logic [15:0] attr,
    input logic [127:0] cfgw,
    input logic [8:0] t_row,
    input logic [9:0] cw,

    output logic mode1_a_req,
    output logic [13:0] mode1_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    output logic mode1_f_req,
    output logic [13:0] mode1_f_addr,
    input logic f_gnt,
    input logic [7:0] f_data,

    /* The plane's palette, still this front's: cells resolve their
     * colors here, and a cell wants its foreground and background at
     * once, so both read ports are its. */
    output logic mode1_pal_ld,
    output logic [7:0] mode1_pal_w,
    output logic [8:0] mode1_pal_words,
    output logic [7:0] mode1_pal_idx_a,
    output logic [7:0] mode1_pal_idx_b,
    output logic mode1_pal_xram,
    output logic mode1_pal_one_bpp,
    input logic [15:0] pal_qa,
    input logic [15:0] pal_qb,

    output logic mode1_tl_start,
    output logic mode1_seg_valid,
    output logic [7:0] mode1_seg_ibits,
    output logic [15:0] mode1_seg_fg,
    output logic [15:0] mode1_seg_bg,
    output logic [9:0] mode1_seg_px,
    input logic seg_take
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
        S1_IDLE, S1_WRAP, S1_ADDR, S1_PAL, S1_SEG
    } state_t;
    state_t state;

    logic signed [16:0] row;
    logic [3:0] scanrow;
    logic [19:0] sizeof_row;
    logic [19:0] row_off;
    logic [16:0] row_base;
    logic signed [16:0] col;
    logic [9:0] px_rem;
    logic blank;

    /* int16 like the oracle: ±32768 wraps before the fold sees it. */
    logic [15:0] row16, col16;
    always_comb row16 = {7'd0, t_row} - 16'(cf_y_pos);
    always_comb col16 = 16'd0 - 16'(cf_x_pos);

    /* The store is the plane's, in palram; this front reloads every
     * entry it will index before it serves a cell. */
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

    /* The cell prefetcher: while the tail emits, the next cell's bytes
     * (up to three words) and its font byte gather here. */
    typedef enum logic [2:0] {
        F_IDLE, F_W, F_FONT, F_READY
    } fstate_t;
    fstate_t fstate;
    logic [16:0] cell_addr;   // byte address of the cell being fetched
    logic [1:0] fw_i, fw_c, fw_n;  // word issue/capture counts
    logic [95:0] gather;           // up to three words, lane-aligned below
    logic [1:0] cell_lane;         // the cell's byte offset in its word
    logic gnt_d;

    logic nxt_v;
    logic [7:0] nxt_bits;
    logic [15:0] nxt_fg, nxt_bg;

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

    logic pal_ld;
    always_comb pal_ld = !abort_i && !start && state == S1_PAL
        && pal_xram && pal_bpp != 4'd0 && gnt_d;
    always_comb begin
        mode1_pal_ld = pal_ld;
        mode1_pal_w = pal_w;
        mode1_pal_words = pal_words;
        mode1_pal_idx_a = fg_idx;
        mode1_pal_idx_b = bg_idx;
        mode1_pal_xram = pal_xram;
        mode1_pal_one_bpp = pal_bpp == 4'd1;
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
        pal_fg = pal_qa;
        pal_bg = pal_qb;
    end

    /* The font byte: gathered from XRAM, or fetched from the store the
     * soft CPU owns. Both arrive through F_FONT, so the built-in path
     * is the one the XRAM fixtures already exercise. */
    logic [7:0] font_gather;
    always_comb mode1_f_addr = fh16
        ? {2'b00, scanrow, g_glyph}
        : {2'b01, 1'b0, scanrow[2:0], g_glyph};
    always_comb mode1_f_req = state == S1_SEG && fstate == F_FONT
        && !font_xram && fw_i == 2'd0;

    /* One channel or the other; font_xram holds for the whole line, so
     * the choice cannot move across a grant. */
    logic f_gnt_d;
    logic fnt_gnt, fnt_gnt_d;
    logic [7:0] fnt_byte;
    always_comb begin
        fnt_gnt = font_xram ? a_gnt : f_gnt;
        fnt_gnt_d = font_xram ? gnt_d : f_gnt_d;
        fnt_byte = font_xram
            ? a_rdata[{font_line_byte, 3'b000}+:8]
            : f_data;
    end

    logic signed [17:0] win_w;
    always_comb win_w = $signed({{2{width_px[15]}}, width_px});

    always_comb begin
        mode1_a_req = 1'b0;
        mode1_a_addr = cell_addr[15:2] + {12'd0, fw_i};
        case (state)
            S1_PAL: begin
                mode1_a_req = pal_xram && pal_bpp != 4'd0
                    && pal_n < pal_fetch;
                mode1_a_addr = cf_palette[15:2] + {5'd0, pal_n};
            end
            S1_SEG: begin
                if (fstate == F_W) begin
                    mode1_a_req = fw_i < fw_n;
                    mode1_a_addr = cell_addr[15:2] + {12'd0, fw_i};
                end else if (fstate == F_FONT) begin
                    mode1_a_req = fw_i == 2'd0;
                    mode1_a_addr = font_line_addr[15:2];
                end
            end
            default: ;
        endcase
    end
    logic [16:0] font_line_addr;
    always_comb font_line_addr = {1'b0, cf_font}
        + {5'd0, scanrow, 8'd0} + {9'd0, g_glyph};

    /* The next segment, purely from where col stands. The entry cell
     * may start mid-glyph; pre-shifting the row puts its first visible
     * pixel at the segment's bit zero, and every cell after it is
     * aligned. */
    logic [16:0] pad_left;
    always_comb pad_left = 17'(-col);
    logic [17:0] run_w;
    always_comb run_w = 18'(win_w - 18'(col));
    logic [3:0] cell_px;
    always_comb cell_px = 4'd8 - {1'b0, col[2:0]};
    always_comb begin
        mode1_seg_valid = 1'b0;
        mode1_seg_ibits = 8'd0;
        mode1_seg_fg = 16'd0;
        mode1_seg_bg = 16'd0;
        mode1_seg_px = px_rem;
        if (state == S1_SEG && px_rem != 10'd0) begin
            if (blank || 18'(col) >= win_w && col >= 0) begin
                mode1_seg_valid = 1'b1;
            end else if (col < 0) begin
                mode1_seg_valid = 1'b1;
                if (pad_left < {7'd0, px_rem})
                    mode1_seg_px = pad_left[9:0];
            end else if (nxt_v) begin
                /* This cell, bounded by the cell, the window and the
                 * line, whichever ends first. */
                mode1_seg_valid = 1'b1;
                mode1_seg_ibits = 8'(nxt_bits << col[2:0]);
                mode1_seg_fg = nxt_fg;
                mode1_seg_bg = nxt_bg;
                mode1_seg_px = {6'd0, cell_px};
                if (px_rem < {6'd0, cell_px})
                    mode1_seg_px = px_rem;
                if (run_w < {8'd0, mode1_seg_px})
                    mode1_seg_px = run_w[9:0];
            end
        end
    end

    initial begin
        state = S1_IDLE;
        fstate = F_IDLE;
        row = '0;
        scanrow = '0;
        sizeof_row = '0;
        row_off = '0;
        row_base = '0;
        col = '0;
        px_rem = '0;
        blank = 1'b0;
        pal_xram = 1'b0;
        pal_n = '0;
        pal_w = '0;
        cell_addr = '0;
        cell_lane = '0;
        fetch_col = '0;
        fw_i = '0;
        fw_c = '0;
        fw_n = '0;
        gather = '0;
        gnt_d = 1'b0;
        f_gnt_d = 1'b0;
        nxt_v = 1'b0;
        nxt_bits = '0;
        nxt_fg = '0;
        nxt_bg = '0;
        font_gather = '0;
        mode1_tl_start = 1'b0;
    end
    always_ff @(posedge clk) begin
        gnt_d <= a_gnt;
        f_gnt_d <= f_gnt;
        mode1_tl_start <= 1'b0;
        if (abort_i) begin
`ifdef VERILATOR
            if (state != S1_IDLE && state != S1_SEG)
                $fatal(1, "mode1 underrun");
`endif
            state <= S1_IDLE;
            fstate <= F_IDLE;
        end else if (start) begin
            row <= $signed({row16[15], row16});
            sizeof_row <= 20'(17'(cf_wchars[15:0])
                              * {14'd0, cell_size});
            col <= $signed({col16[15], col16});
            blank <= 1'b0;
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
                    begin
                        blank <= 1'b1;
                        state <= S1_ADDR;
                    end
                    else if (cf_y_wrap && row < 0)
                        row <= 17'(18'(row) + height_px_s);
                    else if (cf_y_wrap && 18'(row) >= height_px_s)
                        row <= 17'(18'(row) - height_px_s);
                    else if (cf_x_wrap && col < 0)
                        col <= 17'(18'(col) + win_w);
                    else if (cf_x_wrap && 18'(col) >= win_w)
                        col <= 17'(18'(col) - win_w);
                    else if (row < 0 || 18'(row) >= height_px_s)
                    begin
                        blank <= 1'b1;
                        state <= S1_ADDR;
                    end
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
                    if (overrun)
                        blank <= 1'b1;
                    mode1_tl_start <= 1'b1;
                    px_rem <= cw;
                    if (blank || overrun)
                        state <= S1_SEG;
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
                        state <= S1_SEG;
                        fstate <= F_IDLE;
                        fetch_col <= col < 0 ? 16'd0 : col[15:0] >> 3;
                    end else begin
                        if (a_gnt)
                            pal_n <= pal_n + 9'd1;
                        if (gnt_d) begin
                            pal_w <= pal_w + 8'd1;
                            if ({1'b0, pal_w} == pal_fetch - 9'd1)
                            begin
                                state <= S1_SEG;
                                fstate <= F_IDLE;
                                fetch_col <= col < 0
                                    ? 16'd0 : col[15:0] >> 3;
                                pal_w <= '0;
                            end
                        end
                    end
                end
                S1_SEG: begin
                    case (fstate)
                        F_IDLE: begin
                            if (!blank && !nxt_v
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
                                case (fw_c)
                                    2'd0: gather[31:0] <= a_rdata;
                                    2'd1: gather[63:32] <= a_rdata;
                                    2'd2: gather[95:64] <= a_rdata;
                                    default: ;
                                endcase
                                fw_c <= fw_c + 2'd1;
                                if (fw_c + 2'd1 == fw_n) begin
                                    fw_i <= '0;
                                    fstate <= F_FONT;
                                end
                            end
                        end
                        F_FONT: begin
                            if (fnt_gnt)
                                fw_i <= 2'd1;
                            if (fnt_gnt_d) begin
                                font_gather <= fnt_byte;
                                fstate <= F_READY;
                            end
                        end
                        F_READY: begin
                            if (!nxt_v) begin
                                nxt_bits <= font_gather;
                                nxt_fg <= fmt == 3'd4 ? g_fg16 : pal_fg;
                                nxt_bg <= fmt == 3'd4 ? g_bg16 : pal_bg;
                                nxt_v <= 1'b1;
                                fetch_col <= fetch_col + 16'd1;
                                fstate <= F_IDLE;
                            end
                        end
                        default: fstate <= F_IDLE;
                    endcase

                    if (seg_take) begin
                        px_rem <= px_rem - mode1_seg_px;
                        if (px_rem == mode1_seg_px)
                            state <= S1_IDLE;
                        if (!blank && col < 0)
                            col <= col
                                + $signed({7'd0, mode1_seg_px});
                        else if (!blank && 18'(col) < win_w) begin
                            nxt_v <= 1'b0;
                            if (cf_x_wrap
                                && 18'(col) + 18'({8'd0,
                                                   mode1_seg_px})
                                    == win_w) begin
                                col <= '0;
                                fetch_col <= '0;
                                fstate <= F_IDLE;
                            end else
                                col <= col + $signed(
                                    {7'd0, mode1_seg_px});
                        end
                    end
                end
                default: state <= S1_IDLE;
            endcase
        end
    end

    logic [15:0] height_px;
    always_comb height_px = 16'(cf_hchars) << (fh16 ? 4'd4 : 4'd3);
    logic signed [17:0] height_px_s;
    always_comb height_px_s = $signed({{2{height_px[15]}}, height_px});

    /* The grid overruns XRAM; the oracle's reject, folded where the
     * plan latches because it lands on that same edge. */
    logic overrun;
    always_comb overrun = 35'(cf_hchars[14:0]) * 35'(sizeof_row)
        > 35'(17'h10000) - 35'({1'b0, cf_data});

    logic [15:0] fetch_col;
    logic [16:0] cell_fetch_addr;
    always_comb cell_fetch_addr = row_base
        + 17'(fetch_col * 16'({13'd0, cell_size}));
    logic [1:0] font_line_byte;
    always_comb font_line_byte = font_line_addr[1:0];

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_mode1;
    always_comb unused_mode1 = ^{cfgw, attr[15:4], sizeof_row,
                                     row_off[19:17], gather,
                                     win_w[17], cell_addr[16],
                                     cell_addr[1:0],
                                     font_line_addr[16],
                                     pad_left[16:10], run_w[17:10]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
