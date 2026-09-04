/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 2, the tile map of core/vga/mode/mode2.c: rows mapped with true
 * wraparound, the oracle's rejects, the restoring divider that trimmed
 * geometry needs, and one map byte fetched per tile. Each tile's
 * on-screen slice goes to the shared pixel tail as one xram segment — a
 * trim is nothing more than a shorter count — and the tail streams the
 * tile's row bytes itself.
 *
 * The map fetch and the tail's streaming share the plane's one XRAM
 * slot; the wrapper gives the front priority, so the next tile's map
 * byte hides under the current tile's pixels.
 */

module mode2 (
    input logic clk,

    input logic start,
    input logic abort_i,
    input logic [15:0] attr,
    input logic [127:0] cfgw,
    input logic [8:0] t_row,
    input logic [9:0] cw,

    /* The map-byte channel, muxed ahead of the tail's by the wrapper;
     * a_gnt here is only this front's own grants. */
    output logic mode2_a_req,
    output logic [13:0] mode2_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    output logic mode2_tl_start,
    output logic [15:0] mode2_pal_ptr,
    output logic mode2_pal_xram,
    output logic [2:0] mode2_bpp,
    output logic mode2_seg_valid,
    output logic mode2_seg_imm,
    output logic [22:0] mode2_seg_bits,
    output logic [9:0] mode2_seg_px,
    input logic seg_take
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
    always_comb begin
        row_size = 5'(8'({3'd0, tile_size} << bpp_log) >> 3);
        mem_size = 9'({4'd0, row_size} << (tile16 ? 4 : 3));
    end

    /* The oracle computes these in int16, overflow and all. */
    logic [15:0] width_px, height_px;
    always_comb begin
        width_px = 16'(16'(cf_width) * 16'({11'd0, eff_w}));
        height_px = 16'(16'(cf_height) * 16'({11'd0, tile_h}));
    end
    /* Latched at start so the window tests compare registers. */
    logic signed [20:0] win_w_s, height_px_s;

    typedef enum logic [2:0] {
        S2_IDLE, S2_WRAP, S2_DIVY, S2_ADDR, S2_DIVX, S2_SEG
    } state_t;
    state_t state;

    logic signed [20:0] row;
    logic [14:0] q_row;   /* tile row in the map */
    logic [3:0] r_row;    /* row within the tile */
    logic [16:0] row_base;
    logic signed [20:0] col;
    logic [9:0] px_rem;
    logic blank;
    logic [14:0] tile;    /* the tile column being served */
    logic [3:0] tcol;     /* entry column within the first tile */

    /* int16 like the oracle: ±32768 wraps before the fold sees it. */
    logic [15:0] row16, col16;
    always_comb row16 = {7'd0, t_row} - 16'(cf_y_pos);
    always_comb col16 = 16'd0 - 16'(cf_x_pos);

    /* The map overruns XRAM; the oracle's reject, folded where the
     * plan latches because it lands on that same edge. */
    logic overrun;
    always_comb overrun = 35'(cf_height[14:0]) * 35'(cf_width[14:0])
        > 35'(17'h10000) - 35'({1'b0, cf_data});

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

    /* The map byte for the tile being served, held until its segment
     * is taken. Fetching starts the moment the tile is what the line
     * needs next — during the left padding for tile zero, on the take
     * for every tile after — so it hides under emission. */
    typedef enum logic [1:0] {
        M_IDLE, M_REQ, M_WAIT, M_HAVE
    } mstate_t;
    mstate_t mstate;
    logic [7:0] tile_id;
    logic gnt_d;
    logic [16:0] map_addr;
    always_comb map_addr = row_base + {2'd0, tile};
    always_comb begin
        mode2_a_req = state == S2_SEG && mstate == M_REQ;
        mode2_a_addr = map_addr[15:2];
    end

    logic [17:0] tile_row_addr;
    always_comb tile_row_addr = {2'd0, cf_tile}
        + 18'(18'({9'd0, mem_size}) * 18'({10'd0, tile_id}))
        + 18'(18'({13'd0, row_size}) * 18'({14'd0, r_row}));

    logic [20:0] pad_left;
    always_comb pad_left = 21'(-col);
    logic [20:0] run_w;
    always_comb run_w = 21'(win_w_s - col);
    logic [4:0] tile_px;
    always_comb tile_px = eff_w - {1'b0, tcol};
    always_comb begin
        mode2_seg_valid = 1'b0;
        mode2_seg_imm = 1'b1;
        mode2_seg_bits = 23'd0;
        mode2_seg_px = px_rem;
        if (state == S2_SEG && px_rem != 10'd0) begin
            if (blank || col >= win_w_s) begin
                mode2_seg_valid = 1'b1;
            end else if (col < 0) begin
                mode2_seg_valid = 1'b1;
                if (pad_left < {11'd0, px_rem})
                    mode2_seg_px = pad_left[9:0];
            end else if (mstate == M_HAVE) begin
                /* This tile's slice, bounded by the tile, the window
                 * and the line, whichever ends first. */
                mode2_seg_valid = 1'b1;
                mode2_seg_imm = 1'b0;
                mode2_seg_bits = {4'd0, tile_row_addr[15:0], 3'b000}
                    + (23'({19'd0, tcol}) << bpp_log);
                mode2_seg_px = {5'd0, tile_px};
                if ({11'd0, px_rem} < {16'd0, tile_px})
                    mode2_seg_px = px_rem;
                if (run_w < {11'd0, mode2_seg_px})
                    mode2_seg_px = run_w[9:0];
            end
        end
    end

    initial begin
        state = S2_IDLE;
        mstate = M_IDLE;
        row = '0;
        q_row = '0;
        r_row = '0;
        row_base = '0;
        col = '0;
        px_rem = '0;
        blank = 1'b0;
        tile = '0;
        tcol = '0;
        tile_id = '0;
        win_w_s = '0;
        height_px_s = '0;
        div_q = '0;
        div_rem = '0;
        div_i = '0;
        div_den = '0;
        gnt_d = 1'b0;
        mode2_tl_start = 1'b0;
        mode2_pal_ptr = '0;
        mode2_pal_xram = 1'b0;
        mode2_bpp = '0;
    end
    always_ff @(posedge clk) begin
        gnt_d <= a_gnt;
        mode2_tl_start <= 1'b0;
        if (abort_i) begin
`ifdef VERILATOR
            if (state != S2_IDLE && state != S2_SEG)
                $fatal(1, "mode2 underrun");
`endif
            state <= S2_IDLE;
            mstate <= M_IDLE;
        end else if (start) begin
            row <= $signed({{5{row16[15]}}, row16});
            col <= $signed({{5{col16[15]}}, col16});
            win_w_s <= $signed({{5{width_px[15]}}, width_px});
            height_px_s <= $signed({{5{height_px[15]}}, height_px});
            blank <= 1'b0;
            state <= S2_WRAP;
        end else begin
            case (state)
                S2_IDLE: ;
                S2_WRAP: begin
                    /* Iterative wraparound; sane configs settle in a
                     * step or two, and the beam's deadline bounds the
                     * pathological ones. The oracle rejects on the
                     * int16 height, not the tile count. */
                    if (cf_width < 16'sd1 || height_px_s < 21'sd1) begin
                        blank <= 1'b1;
                        state <= S2_ADDR;
                    end else if (cf_y_wrap && row < 0)
                        row <= row + height_px_s;
                    else if (cf_y_wrap && row >= height_px_s)
                        row <= row - height_px_s;
                    else if (cf_x_wrap && col < 0)
                        col <= col + win_w_s;
                    else if (cf_x_wrap && col >= win_w_s)
                        col <= col - win_w_s;
                    else if (row < 0 || row >= height_px_s) begin
                        blank <= 1'b1;
                        state <= S2_ADDR;
                    end else if (y_trim == 4'd0) begin
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
                    if (overrun)
                        blank <= 1'b1;
                    mode2_pal_ptr <= cf_palette;
                    mode2_pal_xram <= !blank && !overrun
                        && !cf_palette[0]
                        && {1'b0, cf_palette}
                            <= 17'h10000
                                - (17'd2 << {12'd0, 5'd1 << bpp_log});
                    mode2_bpp <= {1'b0, bpp_log};
                    mode2_tl_start <= 1'b1;
                    px_rem <= cw;
                    mstate <= M_IDLE;
                    if (blank || overrun || col < 21'sd1) begin
                        tile <= '0;
                        tcol <= '0;
                        state <= S2_SEG;
                    end else if (x_trim == 4'd0) begin
                        tile <= tile16 ? col[18:4] : col[17:3];
                        tcol <= tile16 ? col[3:0] : {1'b0, col[2:0]};
                        state <= S2_SEG;
                    end else begin
                        div_q <= col[19:0];
                        div_rem <= '0;
                        div_i <= '0;
                        div_den <= eff_w;
                        state <= S2_DIVX;
                    end
                end
                S2_DIVX: begin
                    div_q <= div_q_n;
                    div_rem <= div_rem_n;
                    div_i <= div_i + 5'd1;
                    if (div_i == 5'd19) begin
                        tile <= div_q_n[14:0];
                        tcol <= div_rem_n[3:0];
                        state <= S2_SEG;
                    end
                end
                S2_SEG: begin
                    case (mstate)
                        M_IDLE: if (!blank && col < win_w_s)
                            mstate <= M_REQ;
                        M_REQ: if (a_gnt)
                            mstate <= M_WAIT;
                        M_WAIT: if (gnt_d) begin
                            tile_id <= a_rdata[
                                {map_addr[1:0], 3'b000}+:8];
                            mstate <= M_HAVE;
                        end
                        default: ;
                    endcase

                    if (seg_take) begin
                        px_rem <= px_rem - mode2_seg_px;
                        if (px_rem == mode2_seg_px)
                            state <= S2_IDLE;
                        if (!blank && col < 0)
                            col <= col
                                + $signed({11'd0, mode2_seg_px});
                        else if (!blank && col < win_w_s) begin
                            mstate <= M_IDLE;
                            tcol <= '0;
                            if (cf_x_wrap
                                && col + $signed(
                                    {11'd0, mode2_seg_px})
                                    == win_w_s) begin
                                col <= '0;
                                tile <= '0;
                            end else begin
                                col <= col + $signed(
                                    {11'd0, mode2_seg_px});
                                tile <= tile + 15'd1;
                            end
                        end
                    end
                end
                default: state <= S2_IDLE;
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_mode2;
    always_comb unused_mode2 = ^{cfgw, attr[15:12], attr[2],
                                     row[20:19], col[20:19], div_q,
                                     div_rem[5:4], map_addr[16],
                                     tile_row_addr[17:16]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
