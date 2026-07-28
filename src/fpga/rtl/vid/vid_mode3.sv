/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 3, the linear bitmap of vga/modes/mode3.c: rows mapped with true
 * wraparound and the oracle's rejects — range, bitmap overrun, the 16bpp
 * odd row — the palette snapshotted per line as an XRAM burst or the
 * builtin ROM, pixels emitted through the window rules in every bit
 * order, normal and reversed. The plane engine starts it with the config
 * view in hand; it owns the XRAM channel and the pixel port until done.
 * The emitter may stall while the fetch pipeline primes or a wrap rewinds
 * it — only the beam's deadline matters, and an abort_i while busy is the
 * underrun.
 */

module vid_mode3
    import vid_palette_pkg::*;
(
    input logic clk,
    input logic rst_n,

    /* One line of work: start when the config view is valid; abort_i is
     * the next line's deadline. */
    input logic start,
    input logic abort_i,
    input logic [15:0] attr,
    input logic [111:0] cfgw,
    input logic [8:0] t_row,
    input logic [9:0] cw,

    /* The plane's XRAM channel while running. */
    output logic vid_mode3_a_req,
    output logic [13:0] vid_mode3_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    /* Pixels into the plane's write bank. */
    output logic vid_mode3_px_we,
    output logic [9:0] vid_mode3_px_addr,
    output logic [15:0] vid_mode3_px_data,

    /* Done, and whether the plane counts as filled. */
    output logic vid_mode3_done,
    output logic vid_mode3_filled
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

    typedef enum logic [2:0] {
        S3_IDLE, S3_ROW, S3_WRAP, S3_ADDR, S3_PAL, S3_RUN, S3_BLANK
    } state_t;
    state_t state;

    logic signed [16:0] row;
    logic [19:0] sizeof_row;
    logic [19:0] row_off;
    logic [16:0] row_base;
    logic signed [16:0] col;
    logic [9:0] px /*verilator public_flat_rd*/;

    /* The oracle stores these in int16, so ±32768 wraps before the
     * wraparound fold sees it. */
    logic [15:0] row16, col16;
    always_comb row16 = {7'd0, t_row} - 16'(cf_y_pos);
    always_comb col16 = 16'd0 - 16'(cf_x_pos);

    /* Palette: XRAM snapshot or the builtin ROM at emission. */
    /* Read where it is used, so it wants LUT RAM: a block RAM cannot
     * answer without a clock and a register file this wide does not
     * fit. LUT RAM carries one write, and every word of the load
     * carries one even entry and one odd one however the palette is
     * aligned — so the parity split gives each array a single port. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_even[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_odd[128];
    logic pal_xram;
    logic [8:0] pal_n;
    logic [8:0] pal_words;
    /* Entry pairs per word: 2^(bpp-1) words carry the 2^bpp entries. A
     * halfword-aligned palette straddles one more word, entry 0 in the
     * first word's high half. */
    always_comb pal_words = 9'd1 << ((5'd1 << bpp_log) - 5'd1);
    logic [8:0] pal_fetch;
    always_comb pal_fetch = pal_words + {8'd0, cf_palette[1]};
    logic [7:0] pal_w;

    /* The fetch pipeline: two words in flight or banked; issue counts on
     * grant, capture the clock after. */
    logic [31:0] fifo[2];
    logic [1:0] fifo_v;
    logic [1:0] inflight;
    logic [13:0] fetch_word;
    logic gnt_d;
    logic primed;

    logic [4:0] bit_in_word;
    logic [22:0] bit_origin;
    always_comb bit_origin = ({6'd0, row_base} << 3)
        + (23'(col[15:0]) << bpp_log);

    logic [7:0] cur_byte;
    always_comb cur_byte = fifo[0][{bit_in_word[4:3], 3'b000}+:8];
    logic [7:0] pix_idx;
    always_comb begin
        case (bpp_log)
            3'd0: pix_idx = {7'd0, reversed
                ? cur_byte[bit_in_word[2:0]]
                : cur_byte[3'd7 - bit_in_word[2:0]]};
            3'd1: pix_idx = {6'd0, reversed
                ? cur_byte[{bit_in_word[2:1], 1'b0}+:2]
                : cur_byte[{2'd3 - bit_in_word[2:1], 1'b0}+:2]};
            3'd2: pix_idx = {4'd0, reversed
                ? cur_byte[{bit_in_word[2], 2'b00}+:4]
                : cur_byte[{!bit_in_word[2], 2'b00}+:4]};
            default: pix_idx = cur_byte;
        endcase
    end
    logic [15:0] pix16;
    always_comb pix16 = bit_in_word[4] ? fifo[0][31:16] : fifo[0][15:0];
    /* The load's write port, outside the pipeline's reset so it can be
     * memory at all; the conditions are the state machine's own. */
    logic pal_ld;
    always_comb pal_ld = !abort_i && !start && state == S3_PAL
        && pal_xram && bpp_log != 3'd4 && gnt_d;
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
        if (pal_we_e)
            pal_even[pal_w[6:0]] <= pal_wd_e;
        if (pal_we_o)
            pal_odd[pal_wa_o] <= pal_wd_o;
    end

    logic [15:0] pal_ram;
    always_comb pal_ram = pix_idx[0] ? pal_odd[pix_idx[7:1]]
                                     : pal_even[pix_idx[7:1]];
    logic [15:0] pal_out;
    always_comb pal_out = pal_xram ? pal_ram
        : (bpp_log == 3'd0 ? VID_COLOR_2[pix_idx[0]] : VID_COLOR_256[pix_idx]);

    logic in_window;
    always_comb in_window = col >= 0
        && col < $signed({cf_width[15], cf_width});

    /* This clock emits a pixel and it is the word's last. */
    logic fifo_shift;
    always_comb fifo_shift = state == S3_RUN && primed && in_window
        && fifo_v[0]
        && !(cf_x_wrap && col == $signed({cf_width[15], cf_width}) - 17'sd1)
        && 6'(bit_in_word) + {1'b0, 5'd1 << bpp_log} == 6'd32;

    /* Request and address are combinational so a grant always takes the
     * live counter — back-to-back grants must never re-read a word. */
    always_comb begin
        vid_mode3_a_req = 1'b0;
        vid_mode3_a_addr = fetch_word;
        case (state)
            S3_PAL: begin
                vid_mode3_a_req = pal_xram && bpp_log != 3'd4
                    && pal_n < pal_fetch;
                vid_mode3_a_addr = cf_palette[15:2] + {5'd0, pal_n};
            end
            S3_RUN: vid_mode3_a_req = primed && in_window
                && 3'(fifo_v) + 3'(inflight) < 3'd2;
            default: ;
        endcase
    end

    /* The pixel port: emitting, padding outside the window, or blanking. */
    always_comb begin
        vid_mode3_px_we = 1'b0;
        vid_mode3_px_addr = px;
        vid_mode3_px_data = 16'h0000;
        case (state)
            S3_RUN: begin
                if (!in_window)
                    vid_mode3_px_we = 1'b1;
                else if (primed && fifo_v[0]) begin
                    vid_mode3_px_we = 1'b1;
                    vid_mode3_px_data = bpp_log == 3'd4 ? pix16 : pal_out;
                end
            end
            S3_BLANK: vid_mode3_px_we = 1'b1;
            default: ;
        endcase
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S3_IDLE;
            row <= '0;
            sizeof_row <= '0;
            row_off <= '0;
            row_base <= '0;
            col <= '0;
            px <= '0;
            pal_xram <= 1'b0;
            pal_n <= '0;
            pal_w <= '0;
            fifo[0] <= '0;
            fifo[1] <= '0;
            fifo_v <= '0;
            inflight <= '0;
            fetch_word <= '0;
            gnt_d <= 1'b0;
            primed <= 1'b0;
            bit_in_word <= '0;
            vid_mode3_done <= 1'b0;
            vid_mode3_filled <= 1'b0;
        end else begin
            gnt_d <= a_gnt;
            vid_mode3_done <= 1'b0;
            if (abort_i) begin
                if (state != S3_IDLE)
                    $fatal(1, "vid_mode3 underrun");
            end else if (start) begin
                row <= $signed({row16[15], row16});
                sizeof_row <= ((20'(cf_width) << bpp_log) + 20'd7) >> 3;
                col <= $signed({col16[15], col16});
                px <= '0;
                state <= S3_WRAP;
            end else begin
                case (state)
                    S3_IDLE: ;
                    S3_WRAP: begin
                        /* Iterative wraparound; sane configs settle in a
                         * step or two, and the beam's deadline bounds the
                         * pathological ones. */
                        if (cf_width < 16'sd1 || cf_height < 16'sd1)
                            state <= S3_BLANK;
                        else if (cf_y_wrap && row < 0)
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
                            state <= S3_BLANK;
                        else begin
                            row_off <= 20'(37'(row[15:0])
                                           * 37'(sizeof_row));
                            state <= S3_ADDR;
                        end
                    end
                    S3_ADDR: begin
                        row_base <= {1'b0, cf_data} + row_off[16:0];
                        /* Bitmap overrun, and 16bpp rejects an odd row. */
                        if (35'(cf_height[14:0]) * 35'(sizeof_row)
                            > 35'(17'h10000) - 35'({1'b0, cf_data})
                            || (bpp_log == 3'd4
                                && (cf_data[0] ^ row_off[0])))
                            state <= S3_BLANK;
                        else begin
                            pal_xram <= !cf_palette[0]
                                && {1'b0, cf_palette}
                                    <= 17'h10000
                                        - (17'd2 << {12'd0, 5'd1 << bpp_log});
                            pal_n <= '0;
                            pal_w <= '0;
                            state <= S3_PAL;
                        end
                    end
                    S3_PAL: begin
                        if (!pal_xram || bpp_log == 3'd4) begin
                            state <= S3_RUN;
                            primed <= 1'b0;
                            fifo_v <= '0;
                            inflight <= '0;
                        end else begin
                            if (a_gnt)
                                pal_n <= pal_n + 9'd1;
                            if (gnt_d) begin
                                pal_w <= pal_w + 8'd1;
                                if ({1'b0, pal_w} == pal_fetch - 9'd1)
                                begin
                                    state <= S3_RUN;
                                    primed <= 1'b0;
                                    fifo_v <= '0;
                                    inflight <= '0;
                                    pal_w <= '0;
                                end
                            end
                        end
                    end
                    S3_RUN: begin
                        if (!in_window) begin
                            /* Outside the bitmap: transparent black. */
                            px <= px + 10'd1;
                            col <= col + 17'd1;
                            if (col + 17'sd1 == 0)
                                primed <= 1'b0;  /* entering at col 0 */
                            if (px == cw - 10'd1) begin
                                state <= S3_IDLE;
                                vid_mode3_done <= 1'b1;
                                vid_mode3_filled <= 1'b1;
                            end
                        end else if (!primed) begin
                            /* (Re)aim the pipeline at col's bit. */
                            fetch_word <= 14'(bit_origin >> 5);
                            bit_in_word <= 5'(bit_origin & 23'd31);
                            fifo_v <= '0;
                            inflight <= '0;
                            primed <= 1'b1;
                        end else begin
                            /* Keep two words moving; emit when fed. The
                             * capture and the word-boundary shift can land
                             * on the same edge, so they resolve together
                             * through fifo_shift. */
                            if (a_gnt)
                                fetch_word <= fetch_word + 14'd1;
                            inflight <= inflight + (a_gnt ? 2'd1 : 2'd0)
                                - (gnt_d ? 2'd1 : 2'd0);
                            if (fifo_shift) begin
                                if (fifo_v[1]) begin
                                    fifo[0] <= fifo[1];
                                    if (gnt_d)
                                        fifo[1] <= a_rdata;
                                    fifo_v <= {gnt_d, 1'b1};
                                end else begin
                                    if (gnt_d)
                                        fifo[0] <= a_rdata;
                                    fifo_v <= {1'b0, gnt_d};
                                end
                            end else if (gnt_d) begin
                                if (!fifo_v[0])
                                    fifo[0] <= a_rdata;
                                else
                                    fifo[1] <= a_rdata;
                                fifo_v <= {fifo_v[0], 1'b1};
                            end
                            if (fifo_v[0]) begin
                                px <= px + 10'd1;
                                if (cf_x_wrap && col
                                    == $signed({cf_width[15], cf_width})
                                        - 17'sd1)
                                begin
                                    col <= '0;
                                    primed <= 1'b0;
                                end else begin
                                    col <= col + 17'sd1;
                                    if (fifo_shift)
                                        bit_in_word <= '0;
                                    else
                                        bit_in_word <= bit_in_word
                                            + (5'd1 << bpp_log);
                                end
                                if (px == cw - 10'd1) begin
                                    state <= S3_IDLE;
                                    vid_mode3_done <= 1'b1;
                                    vid_mode3_filled <= 1'b1;
                                end
                            end
                        end
                    end
                    S3_BLANK: begin
                        px <= px + 10'd1;
                        if (px == cw - 10'd1) begin
                            state <= S3_IDLE;
                            vid_mode3_done <= 1'b1;
                            vid_mode3_filled <= 1'b0;
                        end
                    end
                    default: state <= S3_IDLE;
                endcase
            end
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_mode3;
    always_comb unused_vid_mode3 = ^{row_off[19:17], sizeof_row, cfgw,
                                     attr[15:4]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
