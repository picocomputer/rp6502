/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One plane's line engine: the RTL image of a mode fill function, working
 * one raster line ahead of the beam into a ping-pong line buffer, the
 * vid_term discipline generalized. Mode 3 so far — the linear bitmap of
 * vga/modes/mode3.c: the config fetched fresh each line, the row mapped
 * with true wraparound, the palette snapshotted per line (an XRAM burst
 * or the builtin ROM), pixels emitted through the window rules — a
 * lead-in and remainder of transparent black where the bitmap does not
 * cover, exact wrap where it does. The emitter may stall while the fetch
 * pipeline primes or a wrap rewinds it; the line has slack, and only the
 * beam's deadline matters, asserted at every line start.
 *
 * On line-doubled canvases a row renders once, on the raster line where
 * it first appears, and the buffer holds through the repeat — scanvideo's
 * y_scale semantics, never a re-render the oracle could not have seen.
 *
 * XRAM port A and the prog-table read port are shared three ways; grants
 * rotate and the engine absorbs the latency.
 */

module vid_mode
    import vid_palette_pkg::*;
(
    input logic clk,
    input logic rst_n,

    input logic [9:0] v,
    input logic [9:0] h,
    input logic line_start,

    /* Latched canvas geometry from vid_prog. */
    input logic x_shift,
    input logic y_shift,
    input logic [9:0] y_offset,

    /* This plane's prog entry: request granted in this plane's slot, the
     * entry registered the clock after. */
    output logic [8:0] vid_mode_p_line,  /* comb: valid whenever granted */
    input logic p_gnt,
    input logic [31:0] p_entry,
    input logic [15:0] p_config,

    /* XRAM port A, arbitrated: hold req with an address; gnt means the
     * address was taken and the word arrives next clock. */
    output logic vid_mode_a_req,
    output logic [13:0] vid_mode_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    /* The beam side: this plane's pixel at h, and whether the line
     * filled at all. */
    output logic [15:0] vid_mode_pix,
    output logic vid_mode_filled
);

    logic [15:0] linebuf[2][640];
    logic wr_bank;
    logic filled_q[2] /*verilator public_flat_rd*/;
    logic flip_next;

    /* The beam reads one ahead of itself. The bank flip lands at the end
     * of h==0, so the reads issued at the ends of h==799 and h==0 — for
     * pixels 0 and 1 — still see a fresh line under its write-side label;
     * a repeat line never flips and reads the held bank throughout. */
    logic [9:0] rd_next;
    always_comb rd_next = x_shift
        ? {1'b0, 9'((h + 10'd1) >> 1)} : h + 10'd1;
    always_ff @(posedge clk) begin
        if (h == 10'd799)
            vid_mode_pix <= linebuf[flip_next ? wr_bank : !wr_bank][10'd0];
        else if (h == 10'd0)
            vid_mode_pix <= linebuf[flip_next ? wr_bank : !wr_bank][rd_next];
        else if (h < 10'd639)
            vid_mode_pix <= linebuf[!wr_bank][rd_next];
        else
            vid_mode_pix <= 16'h0000;
    end
    /* Same nuance as the pixel path: during h==0 a pending flip's fresh
     * bank is still labeled write-side. Elsewhere the flip has landed. */
    always_comb vid_mode_filled =
        filled_q[(h == 10'd0 && flip_next) ? wr_bank : !wr_bank];

    /* Target line: the canvas row it maps to, and whether this raster
     * line starts that row's render. */
    logic [9:0] t /*verilator public_flat_rd*/;
    logic [9:0] t_cv;
    logic render_now;
    always_comb begin
        t_cv = t - y_offset;
        render_now = t >= y_offset && t < 10'd480
            && !(y_shift && t_cv[0]);
    end
    logic [8:0] t_row;
    always_comb t_row = y_shift ? t_cv[9:1] : t_cv[8:0];
    always_comb vid_mode_p_line = t_row;

    typedef enum logic [3:0] {
        S_IDLE, S_PROG, S_PROG_W, S_CFG, S_ROW, S_WRAP, S_ADDR,
        S_PAL, S_RUN, S_BLANK
    } state_t;
    state_t state /*verilator public_flat_rd*/;

    logic [15:0] attr /*verilator public_flat_rd*/;
    logic [15:0] config_ptr /*verilator public_flat_rd*/;

    /* Four fetched words hold the whole 14-byte mode3_config_t; a
     * halfword-aligned pointer just shifts the view. */
    logic [31:0] cfg[4];
    logic [111:0] cfgw;
    always_comb cfgw = 112'({cfg[3], cfg[2], cfg[1], cfg[0]}
                            >> {config_ptr[1], 4'b0000});
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
    logic [2:0] bpp_log;  // log2 of bits per pixel
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

    logic signed [16:0] row;
    logic [19:0] sizeof_row;
    logic [19:0] row_off;
    logic [16:0] row_base;
    logic signed [16:0] col;
    logic [9:0] px /*verilator public_flat_rd*/;
    logic [9:0] cw;
    always_comb cw = x_shift ? 10'd320 : 10'd640;

    /* Palette: XRAM snapshot or the builtin ROM at emission. */
    logic [15:0] palram[256];
    logic pal_xram;
    logic [8:0] pal_n;
    logic [8:0] pal_words;
    /* Entry pairs per word: 2^(bpp-1) words carry the 2^bpp entries. */
    always_comb pal_words = 9'd1 << ((5'd1 << bpp_log) - 5'd1);

    logic [6:0] fifo_v0_pal;  /* palette capture pointer, in words */

    /* The fetch pipeline: two words in flight or banked; issue counts on
     * grant, capture the clock after. */
    logic [31:0] fifo[2];
    logic [1:0] fifo_v;
    logic [1:0] inflight;
    logic [13:0] fetch_word;
    logic gnt_d;
    logic primed;


    logic [4:0] bit_in_word;
    logic [19:0] bit_pos;  // col * bpp, primes the pipeline
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
    logic [15:0] pal_out;
    always_comb pal_out = pal_xram ? palram[pix_idx]
        : (bpp_log == 3'd0 ? VID_COLOR_2[pix_idx[0]] : VID_COLOR_256[pix_idx]);

    logic in_window;
    always_comb in_window = col >= 0
        && col < $signed({cf_width[15], cf_width});

    /* This clock emits a pixel and it is the word's last. */
    logic fifo_shift;
    always_comb fifo_shift = state == S_RUN && primed && in_window
        && fifo_v[0]
        && !(cf_x_wrap && col == $signed({cf_width[15], cf_width}) - 17'sd1)
        && 6'(bit_in_word) + {1'b0, 5'd1 << bpp_log} == 6'd32;

    /* Request and address are combinational so a grant always takes the
     * live counter — back-to-back grants must never re-read a word. */
    always_comb begin
        vid_mode_a_req = 1'b0;
        vid_mode_a_addr = fetch_word;
        case (state)
            S_CFG: begin
                vid_mode_a_req = pal_n < 9'd4;
                vid_mode_a_addr = config_ptr[15:2] + {10'd0, pal_n[3:0]};
            end
            S_PAL: begin
                vid_mode_a_req = pal_xram && bpp_log != 3'd4
                    && pal_n < pal_words;
                vid_mode_a_addr = cf_palette[15:2] + {5'd0, pal_n};
            end
            S_RUN: vid_mode_a_req = primed && in_window
                && 3'(fifo_v) + 3'(inflight) < 3'd2;
            default: ;
        endcase
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wr_bank <= 1'b0;
            filled_q[0] <= 1'b0;
            filled_q[1] <= 1'b0;
            flip_next <= 1'b0;
            state <= S_IDLE;
            t <= '0;
            attr <= '0;
            config_ptr <= '0;
            for (int i = 0; i < 4; i++)
                cfg[i] <= '0;
            row <= '0;
            sizeof_row <= '0;
            row_off <= '0;
            row_base <= '0;
            col <= '0;
            px <= '0;
            pal_xram <= 1'b0;
            pal_n <= '0;
            fifo[0] <= '0;
            fifo[1] <= '0;
            fifo_v <= '0;
            inflight <= '0;
            fetch_word <= '0;
            gnt_d <= 1'b0;
            primed <= 1'b0;
            bit_in_word <= '0;
            bit_pos <= '0;
            fifo_v0_pal <= '0;
        end else begin
            gnt_d <= a_gnt;
            if (line_start) begin
                /* The beam's deadline: a render still short of the end
                 * underran its budget. */
                if ((state == S_RUN || state == S_BLANK) && px != cw)
                    $fatal(1, "vid_mode underrun");
                t <= v == 10'd524 ? 10'd0 : v + 10'd1;
                if (flip_next)
                    wr_bank <= !wr_bank;
                flip_next <= 1'b0;
                state <= S_PROG;
            end else begin
                case (state)
                    S_IDLE: ;
                    S_PROG: begin
                        /* t settled last clock; wait for this plane's
                         * prog slot. */
                        if (!render_now)
                            state <= S_IDLE;
                        else if (p_gnt)
                            state <= S_PROG_W;
                    end
                    S_PROG_W: begin
                        attr <= p_entry[15:0];
                        config_ptr <= p_config;
                        if (!p_entry[31] || p_entry[18:16] != 3'd3) begin
                            /* Nothing programmed, or a mode whose engine
                             * is not built yet: a transparent line. */
                            state <= S_BLANK;
                            px <= '0;
                            flip_next <= 1'b1;
                            filled_q[wr_bank] <= 1'b0;
                        end else begin
                            pal_n <= '0;  /* reuse as the issue counter */
                            fifo_v <= '0;
                            inflight <= '0;
                            state <= S_CFG;
                        end
                    end
                    S_CFG: begin
                        /* Four words from the config pointer; issue on
                         * grant, capture the clock after. */
                        if (a_gnt)
                            pal_n <= pal_n + 9'd1;
                        if (gnt_d) begin
                            cfg[fifo_v] <= a_rdata;
                            fifo_v <= fifo_v + 2'd1;
                            if (fifo_v == 2'd3)
                                state <= S_ROW;
                        end
                    end
                    S_ROW: begin
                        row <= $signed({7'd0, t_row})
                            - $signed({cf_y_pos[15], cf_y_pos});
                        sizeof_row <= ((20'(cf_width) << bpp_log) + 20'd7)
                            >> 3;
                        col <= -$signed({cf_x_pos[15], cf_x_pos});
                        state <= S_WRAP;
                    end
                    S_WRAP: begin
                        /* Iterative wraparound; sane configs settle in a
                         * step or two, and the beam's deadline bounds the
                         * pathological ones. */
                        if (cf_width < 16'sd1 || cf_height < 16'sd1) begin
                            state <= S_BLANK;
                            px <= '0;
                            flip_next <= 1'b1;
                            filled_q[wr_bank] <= 1'b0;
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
                            state <= S_BLANK;
                            px <= '0;
                            flip_next <= 1'b1;
                            filled_q[wr_bank] <= 1'b0;
                        end else begin
                            row_off <= 20'(37'(row[15:0])
                                           * 37'(sizeof_row));
                            state <= S_ADDR;
                        end
                    end
                    S_ADDR: begin
                        row_base <= {1'b0, cf_data} + row_off[16:0];
                        /* Bitmap overrun, and 16bpp rejects an odd row. */
                        if (35'(cf_height[14:0]) * 35'(sizeof_row)
                            > 35'(17'h10000) - 35'({1'b0, cf_data})
                            || (bpp_log == 3'd4
                                && (cf_data[0] ^ row_off[0])))
                        begin
                            state <= S_BLANK;
                            px <= '0;
                            flip_next <= 1'b1;
                            filled_q[wr_bank] <= 1'b0;
                        end else begin
                            pal_xram <= !cf_palette[0]
                                && {1'b0, cf_palette}
                                    <= 17'h10000
                                        - (17'd2 << {12'd0, 5'd1 << bpp_log});
                            pal_n <= '0;
                            state <= S_PAL;
                        end
                    end
                    S_PAL: begin
                        if (!pal_xram || bpp_log == 3'd4) begin
                            state <= S_RUN;
                            px <= '0;
                            primed <= 1'b0;
                            fifo_v <= '0;
                            inflight <= '0;
                        end else begin
                            if (a_gnt)
                                pal_n <= pal_n + 9'd1;
                            if (gnt_d) begin
                                palram[{fifo_v0_pal, 1'b0}] <= a_rdata[15:0];
                                palram[{fifo_v0_pal, 1'b1}] <= a_rdata[31:16];
                                fifo_v0_pal <= fifo_v0_pal + 7'd1;
                                if ({2'd0, fifo_v0_pal} == pal_words - 9'd1)
                                begin
                                    state <= S_RUN;
                                    px <= '0;
                                    primed <= 1'b0;
                                    fifo_v <= '0;
                                    inflight <= '0;
                                    fifo_v0_pal <= '0;
                                end
                            end
                        end
                    end
                    S_RUN: begin
                        if (!in_window) begin
                            /* Outside the bitmap: transparent black. A
                             * wrap can never land here — it resolved at
                             * setup and resets below. */
                            linebuf[wr_bank][px] <= 16'h0000;
                            px <= px + 10'd1;
                            col <= col + 17'd1;
                            if (col + 17'sd1 == 0)
                                primed <= 1'b0;  /* entering at col 0 */
                            if (px == cw - 10'd1) begin
                                state <= S_IDLE;
                                flip_next <= 1'b1;
                                filled_q[wr_bank] <= 1'b1;
                            end
                        end else if (!primed) begin
                            /* (Re)aim the pipeline at col's bit. */
                            bit_pos <= 20'(col[15:0]) << bpp_log;
                            fetch_word <= 14'(bit_origin >> 5);
                            bit_in_word <= 5'(bit_origin & 23'd31);
                            fifo_v <= '0;
                            inflight <= '0;
                            primed <= 1'b1;
                        end else begin
                            /* Keep two words moving; emit when fed. The
                             * capture and the word-boundary shift can land
                             * on the same edge, so they resolve together
                             * below through fifo_shift. */
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
                                linebuf[wr_bank][px] <= bpp_log == 3'd4
                                    ? pix16 : pal_out;
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
                                    state <= S_IDLE;
                                    flip_next <= 1'b1;
                                    filled_q[wr_bank] <= 1'b1;
                                end
                            end
                        end
                    end
                    S_BLANK: begin
                        linebuf[wr_bank][px] <= 16'h0000;
                        px <= px + 10'd1;
                        if (px == cw - 10'd1)
                            state <= S_IDLE;
                    end
                    default: state <= S_IDLE;
                endcase
            end
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_mode;
    always_comb unused_vid_mode = ^{bit_pos, row_off[19:17], sizeof_row,
                                    cfgw, t_cv, p_entry[30:19],
                                    attr[15:4], config_ptr[0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
