/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 2, the tile map of vga/modes/mode2.c: a byte per tile indexing
 * 8x8 or 16x16 tile data at 1, 2, 4 or 8 bits per pixel, optionally
 * trimmed — the on-screen tile loses x_trim right columns and y_trim
 * bottom rows while the data keeps its full stride. The options ride in
 * the prog attr, already validated. Rows map with true wraparound and
 * the oracle's rejects; the palette snapshots per line like mode 3. The
 * next tile's map byte and row bytes gather under the current tile's
 * pixels. Trimmed geometry divides by non-powers of two, so a small
 * restoring divider resolves the row and the mid-window entry column;
 * untrimmed tiles keep the shift path and its zero cost.
 */

module vid_mode2
    import vid_palette_pkg::*;
(
    input logic clk,
    input logic rst_n,

    input logic start,
    input logic abort_i,
    input logic [15:0] attr,
    input logic [127:0] cfgw,
    input logic [8:0] t_row,
    input logic [9:0] cw,

    output logic vid_mode2_a_req,
    output logic [13:0] vid_mode2_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    output logic vid_mode2_px_we,
    output logic [9:0] vid_mode2_px_addr,
    output logic [15:0] vid_mode2_px_data,

    output logic vid_mode2_done,
    output logic vid_mode2_filled
);

    logic cf_x_wrap, cf_y_wrap;
    logic signed [15:0] cf_x_pos, cf_y_pos, cf_width, cf_height;
    logic [15:0] cf_data, cf_palette, cf_tile;
    always_comb begin
        cf_x_wrap = cfgw[7:0] != 8'h00;
        cf_y_wrap = cfgw[15:8] != 8'h00;
        cf_x_pos = cfgw[31:16];
        cf_y_pos = cfgw[47:32];
        cf_width = cfgw[63:48];
        cf_height = cfgw[79:64];
        cf_data = cfgw[95:80];
        cf_palette = cfgw[111:96];
        cf_tile = cfgw[127:112];
    end

    /* The validated options word: depth, tile size, trims. */
    logic tile16;
    logic [1:0] bpp_log;
    logic [3:0] x_trim, y_trim;
    always_comb begin
        bpp_log = attr[1:0];
        tile16 = attr[3];
        x_trim = attr[7:4];
        y_trim = attr[11:8];
    end
    logic [4:0] tile_size, eff_w, tile_h;
    always_comb begin
        tile_size = tile16 ? 5'd16 : 5'd8;
        eff_w = tile_size - {1'b0, x_trim};
        tile_h = tile_size - {1'b0, y_trim};
    end
    logic [4:0] row_size;    /* bytes per stored tile row */
    logic [8:0] mem_size;    /* bytes per stored tile */
    logic [4:0] row_bytes;   /* bytes the on-screen columns span */
    always_comb begin
        row_size = 5'(8'({3'd0, tile_size} << bpp_log) >> 3);
        mem_size = 9'({4'd0, row_size} << (tile16 ? 4 : 3));
        row_bytes = 5'((8'({3'd0, eff_w} << bpp_log) + 8'd7) >> 3);
    end

    /* The oracle computes these in int16, overflow and all. */
    logic [15:0] width_px, height_px;
    always_comb begin
        width_px = 16'(16'(cf_width) * 16'({11'd0, eff_w}));
        height_px = 16'(16'(cf_height) * 16'({11'd0, tile_h}));
    end
    logic signed [20:0] win_w_s, height_px_s;
    always_comb begin
        win_w_s = $signed({{5{width_px[15]}}, width_px});
        height_px_s = $signed({{5{height_px[15]}}, height_px});
    end

    typedef enum logic [3:0] {
        S2_IDLE, S2_WRAP, S2_DIVY, S2_ADDR, S2_DIVX, S2_PAL, S2_RUN, S2_BLANK
    } state_t;
    state_t state;

    logic signed [20:0] row;
    logic [14:0] q_row;   /* tile row in the map */
    logic [3:0] r_row;    /* row within the tile */
    logic [16:0] row_base;
    logic signed [20:0] col;
    logic [9:0] px /*verilator public_flat_rd*/;

    /* int16 like the oracle: ±32768 wraps before the fold sees it. */
    logic [15:0] row16, col16;
    always_comb row16 = {7'd0, t_row} - 16'(cf_y_pos);
    always_comb col16 = 16'd0 - 16'(cf_x_pos);

    /* The restoring divider for trimmed geometry: twenty steps resolve a
     * quotient the shift path cannot. */
    logic [19:0] div_q;
    logic [5:0] div_rem;
    logic [4:0] div_i, div_den;
    logic [5:0] div_t;
    logic div_ge;
    always_comb begin
        div_t = {div_rem[4:0], div_q[19]};
        div_ge = div_t >= {1'b0, div_den};
    end
    logic [19:0] div_q_n;
    logic [5:0] div_rem_n;
    always_comb begin
        div_q_n = {div_q[18:0], div_ge};
        div_rem_n = div_ge ? div_t - {1'b0, div_den} : div_t;
    end

    /* Read where it is used, so it wants LUT RAM: a block RAM
     * cannot answer without a clock and a register file this
     * wide does not fit. */
    /* LUT RAM carries one write, and every word of the load carries
     * one even entry and one odd one however the palette is aligned —
     * so the parity split gives each array a single port. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_even[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] pal_odd[128];
    logic pal_xram;
    logic [8:0] pal_n;
    logic [8:0] pal_words;
    always_comb pal_words = 9'd1 << ((5'd1 << bpp_log) - 5'd1);
    /* A halfword-aligned palette straddles one more word, entry 0 in the
     * first word's high half. */
    logic [8:0] pal_fetch;
    always_comb pal_fetch = pal_words + {8'd0, cf_palette[1]};
    logic [7:0] pal_w;

    /* The tile prefetcher: the next tile's map byte, then its row bytes —
     * up to sixteen across five words — gather while the current tile's
     * pixels emit. */
    typedef enum logic [2:0] {
        F2_IDLE, F2_MAP, F2_SET, F2_W, F2_READY
    } fstate_t;
    fstate_t fstate;
    logic [14:0] fetch_tile;
    logic [7:0] tile_id;
    logic [15:0] taddr;
    logic [1:0] lane;
    logic [2:0] fw_i, fw_c, fw_n;
    logic [159:0] gather;
    logic gnt_d;

    logic [16:0] map_addr;
    always_comb map_addr = row_base + {2'd0, fetch_tile};
    logic [17:0] tile_row_addr;
    always_comb tile_row_addr = {2'd0, cf_tile}
        + 18'(18'({9'd0, mem_size}) * 18'({10'd0, tile_id}))
        + 18'(18'({13'd0, row_size}) * 18'({14'd0, r_row}));
    logic [127:0] gview;
    always_comb gview = 128'(gather >> {lane, 3'b000});

    /* The tile being emitted, and the one waiting. */
    logic nxt_v;
    logic [127:0] nxt_bits;
    logic [127:0] cur_bits;
    logic cur_v;
    logic [3:0] tcol;

    logic [6:0] bit_off;
    always_comb bit_off = 7'({3'd0, tcol} << bpp_log);
    logic [7:0] cur_byte;
    always_comb cur_byte = cur_bits[{bit_off[6:3], 3'b000}+:8];
    logic [7:0] pix_idx;
    always_comb begin
        case (bpp_log)
            2'd0: pix_idx = {7'd0, cur_byte[3'd7 - bit_off[2:0]]};
            2'd1: pix_idx = {6'd0, cur_byte[{2'd3 - bit_off[2:1], 1'b0}+:2]};
            2'd2: pix_idx = {4'd0, cur_byte[{!bit_off[2], 2'b00}+:4]};
            default: pix_idx = cur_byte;
        endcase
    end
    /* The load's write port, outside the pipeline's reset so it can be
     * memory at all; the conditions are the state machine's own. */
    logic pal_ld;
    always_comb pal_ld = !abort_i && !start && state == S2_PAL
        && pal_xram && gnt_d;
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
        : (bpp_log == 2'd0 ? VID_COLOR_2[pix_idx[0]] : VID_COLOR_256[pix_idx]);

    logic in_window;
    always_comb in_window = col >= 0 && col < win_w_s;

    always_comb begin
        vid_mode2_a_req = 1'b0;
        vid_mode2_a_addr = taddr[15:2] + {11'd0, fw_i};
        case (state)
            S2_PAL: begin
                vid_mode2_a_req = pal_xram && pal_n < pal_fetch;
                vid_mode2_a_addr = cf_palette[15:2] + {5'd0, pal_n};
            end
            S2_RUN: begin
                if (fstate == F2_MAP) begin
                    vid_mode2_a_req = fw_i == 3'd0;
                    vid_mode2_a_addr = map_addr[15:2];
                end else if (fstate == F2_W)
                    vid_mode2_a_req = fw_i < fw_n;
            end
            default: ;
        endcase
    end

    /* The pixel port: emitting, padding outside the window, blanking. */
    always_comb begin
        vid_mode2_px_we = 1'b0;
        vid_mode2_px_addr = px;
        vid_mode2_px_data = 16'h0000;
        if (state == S2_RUN && !in_window)
            vid_mode2_px_we = 1'b1;
        else if (state == S2_RUN && cur_v) begin
            vid_mode2_px_we = 1'b1;
            vid_mode2_px_data = pal_out;
        end else if (state == S2_BLANK)
            vid_mode2_px_we = 1'b1;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S2_IDLE;
            fstate <= F2_IDLE;
            row <= '0;
            q_row <= '0;
            r_row <= '0;
            row_base <= '0;
            col <= '0;
            px <= '0;
            div_q <= '0;
            div_rem <= '0;
            div_i <= '0;
            div_den <= '0;
            pal_xram <= 1'b0;
            pal_n <= '0;
            pal_w <= '0;
            fetch_tile <= '0;
            tile_id <= '0;
            taddr <= '0;
            lane <= '0;
            fw_i <= '0;
            fw_c <= '0;
            fw_n <= '0;
            gather <= '0;
            gnt_d <= 1'b0;
            nxt_v <= 1'b0;
            nxt_bits <= '0;
            cur_v <= 1'b0;
            cur_bits <= '0;
            tcol <= '0;
            vid_mode2_done <= 1'b0;
            vid_mode2_filled <= 1'b0;
        end else begin
            gnt_d <= a_gnt;
            vid_mode2_done <= 1'b0;
            if (abort_i) begin
                if (state != S2_IDLE)
                    $fatal(1, "vid_mode2 underrun px=%0d col=%0d st=%0d f=%0d nxt=%0d cur=%0d ft=%0d fw=%0d/%0d/%0d ta=%04X id=%0d",
                           px, col, state, fstate, nxt_v, cur_v, fetch_tile,
                           fw_i, fw_c, fw_n, taddr, tile_id);
            end else if (start) begin
                row <= $signed({{5{row16[15]}}, row16});
                col <= $signed({{5{col16[15]}}, col16});
                px <= '0;
                cur_v <= 1'b0;
                nxt_v <= 1'b0;
                tcol <= '0;
                fstate <= F2_IDLE;
                state <= S2_WRAP;
            end else begin
                case (state)
                    S2_IDLE: ;
                    S2_WRAP: begin
                        /* Iterative wraparound; sane configs settle in a
                         * step or two, and the beam's deadline bounds the
                         * pathological ones. The oracle rejects on the
                         * int16 height, not the tile count. */
                        if (cf_width < 16'sd1 || height_px_s < 21'sd1)
                            state <= S2_BLANK;
                        else if (cf_y_wrap && row < 0)
                            row <= row + height_px_s;
                        else if (cf_y_wrap && row >= height_px_s)
                            row <= row - height_px_s;
                        else if (cf_x_wrap && col < 0)
                            col <= col + win_w_s;
                        else if (cf_x_wrap && col >= win_w_s)
                            col <= col - win_w_s;
                        else if (row < 0 || row >= height_px_s)
                            state <= S2_BLANK;
                        else if (y_trim == 4'd0) begin
                            q_row <= tile16 ? row[18:4] : row[17:3];
                            r_row <= tile16 ? row[3:0] : {1'b0, row[2:0]};
                            state <= S2_ADDR;
                        end else begin
                            div_q <= row[19:0];
                            div_rem <= '0;
                            div_i <= '0;
                            div_den <= tile_h;
                            state <= S2_DIVY;
                        end
                    end
                    S2_DIVY: begin
                        div_q <= div_q_n;
                        div_rem <= div_rem_n;
                        div_i <= div_i + 5'd1;
                        if (div_i == 5'd19) begin
                            q_row <= div_q_n[14:0];
                            r_row <= div_rem_n[3:0];
                            state <= S2_ADDR;
                        end
                    end
                    S2_ADDR: begin
                        row_base <= {1'b0, cf_data}
                            + 17'(17'(q_row) * 17'({2'd0, cf_width[14:0]}));
                        /* The map overruns XRAM. */
                        if (35'(cf_height[14:0]) * 35'(cf_width[14:0])
                            > 35'(17'h10000) - 35'({1'b0, cf_data}))
                            state <= S2_BLANK;
                        else begin
                            pal_xram <= !cf_palette[0]
                                && {1'b0, cf_palette}
                                    <= 17'h10000
                                        - (17'd2 << {12'd0, 5'd1 << bpp_log});
                            pal_n <= '0;
                            pal_w <= '0;
                            if (col < 21'sd1) begin
                                fetch_tile <= '0;
                                tcol <= '0;
                                state <= S2_PAL;
                            end else if (x_trim == 4'd0) begin
                                fetch_tile <= tile16
                                    ? col[18:4] : col[17:3];
                                tcol <= tile16
                                    ? col[3:0] : {1'b0, col[2:0]};
                                state <= S2_PAL;
                            end else begin
                                div_q <= col[19:0];
                                div_rem <= '0;
                                div_i <= '0;
                                div_den <= eff_w;
                                state <= S2_DIVX;
                            end
                        end
                    end
                    S2_DIVX: begin
                        div_q <= div_q_n;
                        div_rem <= div_rem_n;
                        div_i <= div_i + 5'd1;
                        if (div_i == 5'd19) begin
                            fetch_tile <= div_q_n[14:0];
                            tcol <= div_rem_n[3:0];
                            state <= S2_PAL;
                        end
                    end
                    S2_PAL: begin
                        if (!pal_xram) begin
                            state <= S2_RUN;
                            fstate <= F2_IDLE;
                        end else begin
                            if (a_gnt)
                                pal_n <= pal_n + 9'd1;
                            if (gnt_d) begin
                                pal_w <= pal_w + 8'd1;
                                if ({1'b0, pal_w} == pal_fetch - 9'd1)
                                begin
                                    state <= S2_RUN;
                                    fstate <= F2_IDLE;
                                    pal_w <= '0;
                                end
                            end
                        end
                    end
                    S2_RUN: begin
                        /* The fetcher gathers the next tile. */
                        case (fstate)
                            F2_IDLE: begin
                                if (!nxt_v
                                    && fetch_tile < cf_width[14:0]) begin
                                    fw_i <= '0;
                                    fstate <= F2_MAP;
                                end
                            end
                            F2_MAP: begin
                                if (a_gnt)
                                    fw_i <= 3'd1;
                                if (gnt_d) begin
                                    tile_id <= a_rdata[
                                        {map_addr[1:0], 3'b000}+:8];
                                    fstate <= F2_SET;
                                end
                            end
                            F2_SET: begin
                                taddr <= tile_row_addr[15:0];
                                lane <= tile_row_addr[1:0];
                                fw_i <= '0;
                                fw_c <= '0;
                                fw_n <= 3'((5'({3'd0, tile_row_addr[1:0]})
                                            + row_bytes + 5'd3) >> 2);
                                gather <= '0;
                                fstate <= F2_W;
                            end
                            F2_W: begin
                                if (a_gnt)
                                    fw_i <= fw_i + 3'd1;
                                if (gnt_d) begin
                                    gather <= gather
                                        | (160'(a_rdata) << {fw_c, 5'd0});
                                    fw_c <= fw_c + 3'd1;
                                    if (fw_c + 3'd1 == fw_n)
                                        fstate <= F2_READY;
                                end
                            end
                            F2_READY: begin
                                if (!nxt_v) begin
                                    nxt_bits <= gview;
                                    nxt_v <= 1'b1;
                                    fetch_tile <= fetch_tile + 15'd1;
                                    fstate <= F2_IDLE;
                                end
                            end
                            default: fstate <= F2_IDLE;
                        endcase

                        /* The emitter. */
                        if (!in_window) begin
                            px <= px + 10'd1;
                            col <= col + 21'sd1;
                            if (col + 21'sd1 == 0) begin
                                /* Entering the window: the lead-in's
                                 * prefetch targeted tile 0. */
                                cur_v <= 1'b0;
                            end
                            if (px == cw - 10'd1)
                                line_end(1'b1);
                        end else if (!cur_v) begin
                            if (nxt_v) begin
                                cur_bits <= nxt_bits;
                                cur_v <= 1'b1;
                                nxt_v <= 1'b0;
                            end
                        end else begin
                            px <= px + 10'd1;
                            if (cf_x_wrap && col == win_w_s - 21'sd1) begin
                                col <= '0;
                                tcol <= '0;
                                cur_v <= 1'b0;
                                nxt_v <= 1'b0;
                                fetch_tile <= '0;
                                fstate <= F2_IDLE;
                            end else begin
                                col <= col + 21'sd1;
                                if ({1'b0, tcol} == eff_w - 5'd1) begin
                                    tcol <= '0;
                                    cur_v <= 1'b0;  /* next tile loads */
                                end else
                                    tcol <= tcol + 4'd1;
                            end
                            if (px == cw - 10'd1)
                                line_end(1'b1);
                        end
                    end
                    S2_BLANK: begin
                        px <= px + 10'd1;
                        if (px == cw - 10'd1)
                            line_end(1'b0);
                    end
                    default: state <= S2_IDLE;
                endcase
            end
        end
    end

    task automatic line_end(input logic filled);
        state <= S2_IDLE;
        vid_mode2_done <= 1'b1;
        vid_mode2_filled <= filled;
    endtask

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_mode2;
    always_comb unused_vid_mode2 = ^{cfgw, attr[15:12], attr[2],
                                     row[20:19], col[20:19], div_q,
                                     div_rem[5:4], gather, map_addr[16],
                                     tile_row_addr[17:16], taddr[1:0],
                                     bit_off};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
