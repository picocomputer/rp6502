/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 1, the character cells of core/vga/mode/mode1.c: the cell gather,
 * the font fetch from XRAM or the builtin store, and each cell's two
 * colors resolved through the plane's palette, in the line the shared row
 * mapper places. A font row is a 1bpp bitmap and a cell's
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

    /* What the shared row mapper needs from this mode, and its view of
     * the line in return. */
    output logic [4:0] mode1_win_wf,
    output logic [4:0] mode1_win_hf,
    output logic [19:0] mode1_sizeof_row,
    output logic mode1_addr,
    output logic [14:0] mode1_data_row,
    output logic mode1_seg_on,
    output logic mode1_run_ready,
    output logic [9:0] mode1_run_max,
    input logic rm_settle,
    input logic signed [16:0] rm_row,
    input logic signed [16:0] rm_col,
    input logic [16:0] rm_row_base,
    input logic rm_blank,
    input logic rm_overrun,
    input logic rm_run,
    input logic rm_wrap,
    input logic rm_end,

    output logic mode1_a_req,
    output logic [13:0] mode1_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    output logic mode1_f_req,
    output logic [13:0] mode1_f_addr,
    input logic f_gnt,
    input logic [7:0] f_data,

    /* The plane's palette, which the pixel tail loads at the palette's
     * depth before the first cell. Cells resolve their colors here, and
     * a cell wants its foreground and background at once, so both read
     * ports are its. */
    output logic [2:0] mode1_bpp,
    input logic pal_done,
    output logic [7:0] mode1_pal_idx_a,
    output logic [7:0] mode1_pal_idx_b,
    input logic [15:0] pal_qa,
    input logic [15:0] pal_qb,

    output logic [7:0] mode1_seg_ibits,
    output logic [15:0] mode1_seg_fg,
    output logic [15:0] mode1_seg_bg,
    input logic seg_take
);

    logic signed [15:0] cf_wchars;
    logic [15:0] cf_font;
    always_comb begin
        cf_wchars = cfgw[63:48];
        cf_font = cfgw[127:112];
    end

    /* attr[3] picks the 8x16 font; attr[2:0] the cell format. */
    logic fh16;
    logic [2:0] fmt;
    logic [2:0] cell_size;
    always_comb begin
        fh16 = attr[3];
        fmt = attr[2:0];
        /* The palette's depth as a logarithm; 4 is raw color. */
        case (fmt)
            3'd0: begin cell_size = 3'd1; mode1_bpp = 3'd0; end
            3'd1: begin cell_size = 3'd2; mode1_bpp = 3'd2; end
            3'd2: begin cell_size = 3'd2; mode1_bpp = 3'd2; end
            3'd3: begin cell_size = 3'd3; mode1_bpp = 3'd3; end
            default: begin cell_size = 3'd6; mode1_bpp = 3'd4; end
        endcase
    end
    /* The oracle computes these in int16, overflow and all. */
    logic [15:0] width_px;
    always_comb width_px = 16'(cf_wchars) << 3;

    typedef enum logic [2:0] {
        S1_IDLE, S1_WRAP, S1_ADDR, S1_PAL, S1_SEG
    } state_t;
    state_t state;

    logic [3:0] scanrow;
    logic [19:0] sizeof_row;

    /* The font fits below the top of XRAM, and a font is 2 or 4 KB, so
     * the pointer clears the limit unless it lands in that last block:
     * the bits above the block all ones and something inside it set.
     * The built-in font's $FFFF sentinel fails that test, as it must. */
    logic font_xram;
    always_comb font_xram = fh16
        ? !(&cf_font[15:12] && |cf_font[11:0])
        : !(&cf_font[15:11] && |cf_font[10:0]);

    /* The cell prefetcher, two stages deep: the word stage gathers a
     * cell's bytes (up to three words) while the font stage fetches the
     * glyph row of the cell before it, so a cell costs the longer fetch
     * rather than the two in series. The tail lands eight pixels in four
     * clocks, and a stage keeps up with that. */
    typedef enum logic [0:0] {
        W_IDLE, W_WORDS
    } wstate_t;
    wstate_t wstate;
    logic [16:0] cell_addr;   // byte address of the cell being fetched
    logic [1:0] fw_i, fw_c, fw_n;  // word issue/capture counts
    logic [95:0] gather;           // up to three words, lane-aligned below
    logic [1:0] cell_lane;         // the cell's byte offset in its word
    logic gw_v;                    // gather holds a cell not yet taken
    /* XRAM returns a word two clocks after the grant; the font store, one. */
    logic gnt_w_d1, gnt_w_d, gnt_f_d1, gnt_f_d;

    typedef enum logic [1:0] {
        F_IDLE, F_FONT, F_HOLD
    } fstate_t;
    fstate_t fstate;
    logic f_sent;
    logic [7:0] gf_glyph, gf_b1, gf_b2, gf_bits;
    logic [15:0] gf_fg16, gf_bg16;

    logic nxt_v;
    logic [7:0] nxt_bits;
    logic [15:0] nxt_fg, nxt_bg;

    logic [47:0] gview;
    always_comb gview = 48'(gather >> {cell_lane, 3'b000});

    always_comb begin
        mode1_pal_idx_a = fg_idx;
        mode1_pal_idx_b = bg_idx;
    end

    logic [15:0] pal_fg, pal_bg;
    logic [7:0] fg_idx, bg_idx;
    always_comb begin
        case (fmt)
            3'd0: begin fg_idx = 8'd1; bg_idx = 8'd0; end
            3'd1: begin fg_idx = {4'd0, gf_b1[7:4]}; bg_idx = {4'd0, gf_b1[3:0]}; end
            3'd2: begin fg_idx = {4'd0, gf_b1[3:0]}; bg_idx = {4'd0, gf_b1[7:4]}; end
            default: begin fg_idx = gf_b1; bg_idx = gf_b2; end
        endcase
        pal_fg = pal_qa;
        pal_bg = pal_qb;
    end

    always_comb mode1_f_addr = fh16
        ? {2'b00, scanrow, gf_glyph}
        : {2'b01, 1'b0, scanrow[2:0], gf_glyph};
    always_comb mode1_f_req = state == S1_SEG && fstate == F_FONT
        && !font_xram && !f_sent;

    /* Both stages fetch from XRAM when the font is there. The font stage
     * is ahead in the pipe, so it goes first. The word stage asks for a
     * cell's first word on the clock it takes the cell, so the round
     * trip does not sit in series with the take. */
    logic w_take, w_want, f_want, gnt_w, gnt_f;
    always_comb begin
        w_want = state == S1_SEG
            && ((wstate == W_WORDS && fw_i < fw_n) || w_take);
        f_want = state == S1_SEG && fstate == F_FONT && font_xram
            && !f_sent;
        gnt_f = a_gnt && f_want;
        gnt_w = a_gnt && !f_want && w_want;
    end

    /* One channel or the other; font_xram holds for the whole line, so
     * the choice cannot move across a grant. */
    logic f_gnt_d;
    logic fnt_gnt, fnt_gnt_d;
    logic [7:0] fnt_byte;
    always_comb begin
        fnt_gnt = font_xram ? gnt_f : f_gnt;
        fnt_gnt_d = font_xram ? gnt_f_d : f_gnt_d;
        fnt_byte = font_xram
            ? a_rdata[{font_line_byte, 3'b000}+:8]
            : f_data;
    end
    logic f_take;
    always_comb f_take = state == S1_SEG && fstate == F_IDLE && gw_v;

    logic signed [17:0] win_w;
    always_comb win_w = $signed({{2{width_px[15]}}, width_px});
    always_comb w_take = state == S1_SEG && wstate == W_IDLE && !rm_blank
        && (!gw_v || f_take)
        && $signed({3'd0, fetch_col, 3'b000}) < win_w;

    always_comb begin
        mode1_a_req = 1'b0;
        mode1_a_addr = cell_addr[15:2] + {12'd0, fw_i};
        case (state)
            S1_SEG: begin
                if (f_want) begin
                    mode1_a_req = 1'b1;
                    mode1_a_addr = font_line_addr[15:2];
                end else if (w_want) begin
                    mode1_a_req = 1'b1;
                    mode1_a_addr = wstate == W_IDLE
                        ? cell_fetch_addr[15:2]
                        : cell_addr[15:2] + {12'd0, fw_i};
                end
            end
            default: ;
        endcase
    end
    logic [16:0] font_line_addr;
    always_comb font_line_addr = {1'b0, cf_font}
        + {5'd0, scanrow, 8'd0} + {9'd0, gf_glyph};

    /* The next cell of the run, from where the walk stands. The entry cell
     * may start mid-glyph, so the row is shifted left to put its first
     * visible pixel on bit 7, which pixtail.sv emits first. Every cell
     * after it is aligned. */
    logic [3:0] cell_px;
    always_comb cell_px = 4'd8 - {1'b0, rm_col[2:0]};
    always_comb begin
        mode1_win_wf = 5'd8;
        mode1_win_hf = fh16 ? 5'd16 : 5'd8;
        mode1_sizeof_row = sizeof_row;
        mode1_addr = state == S1_ADDR;
        mode1_data_row = fh16 ? {4'd0, rm_row[14:4]} : {3'd0, rm_row[14:3]};
        mode1_seg_on = state == S1_SEG;
        mode1_run_ready = nxt_v;
        mode1_run_max = {6'd0, cell_px};
    end
    always_comb begin
        mode1_seg_ibits = rm_run ? 8'(nxt_bits << rm_col[2:0]) : 8'd0;
        mode1_seg_fg = rm_run ? nxt_fg : 16'd0;
        mode1_seg_bg = rm_run ? nxt_bg : 16'd0;
    end

    initial begin
        state = S1_IDLE;
        fstate = F_IDLE;
        scanrow = '0;
        sizeof_row = '0;
        cell_addr = '0;
        cell_lane = '0;
        fetch_col = '0;
        fw_i = '0;
        fw_c = '0;
        fw_n = '0;
        gather = '0;
        gw_v = 1'b0;
        gnt_w_d1 = 1'b0;
        gnt_w_d = 1'b0;
        gnt_f_d1 = 1'b0;
        gnt_f_d = 1'b0;
        f_gnt_d = 1'b0;
        wstate = W_IDLE;
        f_sent = 1'b0;
        gf_glyph = '0;
        gf_b1 = '0;
        gf_b2 = '0;
        gf_bits = '0;
        gf_fg16 = '0;
        gf_bg16 = '0;
        nxt_v = 1'b0;
        nxt_bits = '0;
        nxt_fg = '0;
        nxt_bg = '0;
    end
    always_ff @(posedge clk) begin
        gnt_w_d1 <= gnt_w;
        gnt_w_d <= gnt_w_d1;
        gnt_f_d1 <= gnt_f;
        gnt_f_d <= gnt_f_d1;
        f_gnt_d <= f_gnt;
        if (abort_i) begin
`ifdef VERILATOR
            if (state != S1_IDLE && state != S1_SEG)
                $fatal(1, "mode1 underrun");
`endif
            state <= S1_IDLE;
            wstate <= W_IDLE;
            gw_v <= 1'b0;
            fstate <= F_IDLE;
        end else if (start) begin
            sizeof_row <= 20'(17'(cf_wchars[15:0])
                              * {14'd0, cell_size});
            nxt_v <= 1'b0;
            wstate <= W_IDLE;
            gw_v <= 1'b0;
            fstate <= F_IDLE;
            state <= S1_WRAP;
            fetch_col <= '0;
        end else begin
            case (state)
                S1_IDLE: ;
                S1_WRAP:
                    if (rm_settle) begin
                        scanrow <= fh16 ? rm_row[3:0] : {1'b0, rm_row[2:0]};
                        state <= S1_ADDR;
                    end
                S1_ADDR:
                    state <= rm_blank || rm_overrun ? S1_SEG : S1_PAL;
                S1_PAL:
                    if (pal_done) begin
                        state <= S1_SEG;
                        fstate <= F_IDLE;
                        fetch_col <= rm_col < 0 ? 12'd0 : rm_col[14:3];
                    end
                S1_SEG: begin
                    case (wstate)
                        W_IDLE: begin
                            if (w_take) begin
                                cell_addr <= cell_fetch_addr;
                                cell_lane <= cell_fetch_addr[1:0];
                                fw_i <= gnt_w ? 2'd1 : 2'd0;
                                fw_c <= '0;
                                fw_n <= 2'((4'(cell_fetch_addr[1:0])
                                     + {1'd0, cell_size} + 4'd3) >> 2);
                                gather <= '0;
                                wstate <= W_WORDS;
                            end
                        end
                        default: begin
                            if (gnt_w)
                                fw_i <= fw_i + 2'd1;
                            if (gnt_w_d) begin
                                case (fw_c)
                                    2'd0: gather[31:0] <= a_rdata;
                                    2'd1: gather[63:32] <= a_rdata;
                                    2'd2: gather[95:64] <= a_rdata;
                                    default: ;
                                endcase
                                fw_c <= fw_c + 2'd1;
                                if (fw_c + 2'd1 == fw_n) begin
                                    gw_v <= 1'b1;
                                    fetch_col <= fetch_col + 12'd1;
                                    wstate <= W_IDLE;
                                end
                            end
                        end
                    endcase

                    case (fstate)
                        F_IDLE: begin
                            if (f_take) begin
                                gf_glyph <= gview[7:0];
                                gf_b1 <= gview[15:8];
                                gf_b2 <= gview[23:16];
                                gf_fg16 <= gview[31:16];
                                gf_bg16 <= gview[47:32];
                                gw_v <= 1'b0;
                                f_sent <= 1'b0;
                                fstate <= F_FONT;
                            end
                        end
                        F_FONT: begin
                            if (fnt_gnt)
                                f_sent <= 1'b1;
                            if (fnt_gnt_d) begin
                                if (!nxt_v) begin
                                    nxt_bits <= fnt_byte;
                                    nxt_fg <= fmt == 3'd4 ? gf_fg16 : pal_fg;
                                    nxt_bg <= fmt == 3'd4 ? gf_bg16 : pal_bg;
                                    nxt_v <= 1'b1;
                                    fstate <= F_IDLE;
                                end else begin
                                    gf_bits <= fnt_byte;
                                    fstate <= F_HOLD;
                                end
                            end
                        end
                        default: begin
                            if (!nxt_v) begin
                                nxt_bits <= gf_bits;
                                nxt_fg <= fmt == 3'd4 ? gf_fg16 : pal_fg;
                                nxt_bg <= fmt == 3'd4 ? gf_bg16 : pal_bg;
                                nxt_v <= 1'b1;
                                fstate <= F_IDLE;
                            end
                        end
                    endcase

                    if (rm_end)
                        state <= S1_IDLE;
                    if (seg_take && rm_run) begin
                        nxt_v <= 1'b0;
                        if (rm_wrap) begin
                            fetch_col <= '0;
                            wstate <= W_IDLE;
                            gw_v <= 1'b0;
                            fstate <= F_IDLE;
                        end
                    end
                end
                default: state <= S1_IDLE;
            endcase
        end
    end

    /* The cell the word stage fetches next, under 4096 because a row is
     * at most 32760 pixels, an int16 width that is a multiple of 8. */
    logic [11:0] fetch_col;
    logic [16:0] cell_fetch_addr;
    always_comb cell_fetch_addr = rm_row_base
        + 17'(fetch_col * {9'd0, cell_size});
    logic [1:0] font_line_byte;
    always_comb font_line_byte = font_line_addr[1:0];

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_mode1;
    always_comb unused_mode1 = ^{cfgw, attr[15:4], sizeof_row,
                                     gather,
                                     win_w[17], cell_addr[16],
                                     cell_addr[1:0],
                                     font_line_addr[16],
                                     rm_row[16:15], rm_col[15]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
