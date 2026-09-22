/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 2, the tile map of core/vga/mode/mode2.c: the restoring divider
 * that trimmed geometry needs and one map byte fetched per tile, in the
 * line the shared row mapper places. Each tile's
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

    /* What the shared row mapper needs from this mode, and its view of
     * the line in return. */
    output logic [4:0] mode2_win_wf,
    output logic [4:0] mode2_win_hf,
    output logic [19:0] mode2_sizeof_row,
    output logic mode2_addr,
    output logic [14:0] mode2_data_row,
    output logic mode2_seg_on,
    output logic mode2_run_ready,
    output logic [9:0] mode2_run_max,
    input logic rm_settle,
    input logic rm_reject,
    input logic signed [16:0] rm_row,
    input logic signed [16:0] rm_col,
    input logic [16:0] rm_row_base,
    input logic rm_blank,
    input logic rm_overrun,
    input logic rm_run,
    input logic rm_right,
    input logic rm_wrap,
    input logic rm_end,

    /* The map-byte channel, muxed ahead of the tail's by the wrapper;
     * a_gnt here is only this front's own grants. */
    output logic mode2_a_req,
    output logic [13:0] mode2_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    output logic [2:0] mode2_bpp,
    output logic mode2_seg_imm,
    output logic [22:0] mode2_seg_bits,
    input logic seg_take
);

    logic signed [15:0] cf_width;
    logic [15:0] cf_tile;
    always_comb begin
        cf_width = cfgw[63:48];
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


    typedef enum logic [2:0] {
        S2_IDLE, S2_WRAP, S2_DIVY, S2_ADDR, S2_DIVX, S2_SEG
    } state_t;
    state_t state;

    logic [14:0] q_row;   /* tile row in the map */
    logic [3:0] r_row;    /* row within the tile */
    logic [14:0] tile;    /* the tile column being served */
    logic [3:0] tcol;     /* entry column within the first tile */

    /* The restoring divider for trimmed geometry, which the shift path
     * cannot resolve. The row and column it divides are int16 values
     * folded toward zero, never above 32767, so fifteen steps do. */
    logic [14:0] div_q;
    logic [5:0] div_rem;
    logic [4:0] div_den;
    logic [3:0] div_i;
    logic [5:0] div_t;
    logic div_ge;
    always_comb begin
        div_t = {div_rem[4:0], div_q[14]};
        div_ge = div_t >= {1'b0, div_den};
    end
    logic [14:0] div_q_n;
    logic [5:0] div_rem_n;
    always_comb begin
        div_q_n = {div_q[13:0], div_ge};
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
    logic gnt_d1, gnt_d;
    logic [16:0] map_addr;
    always_comb map_addr = rm_row_base + {2'd0, tile};
    /* The last map word fetched, which holds four tile ids: the next
     * three tiles along a row are in it already, and only the fourth
     * goes back to XRAM. */
    logic map_v;
    logic [13:0] map_wq;
    logic [31:0] map_q;
    logic map_hit;
    always_comb map_hit = map_v && map_wq == map_addr[15:2];
    always_comb begin
        mode2_a_req = state == S2_SEG && mstate == M_REQ;
        mode2_a_addr = map_addr[15:2];
    end

    logic [17:0] tile_row_addr;
    always_comb tile_row_addr = {2'd0, cf_tile}
        + 18'(18'({9'd0, mem_size}) * 18'({10'd0, tile_id}))
        + 18'(18'({13'd0, row_size}) * 18'({14'd0, r_row}));

    /* Mode 2 does not require a full tile set in XRAM, so a tile id can name
     * a row that runs off the end of it. That tile is not drawn. row_size
     * takes one of five values, so the address the row may not pass is one
     * of five constants and the sum needs no adder. */
    logic [17:0] tile_limit;
    always_comb
        case (row_size)
            5'd1: tile_limit = 18'h0FFFF;
            5'd2: tile_limit = 18'h0FFFE;
            5'd4: tile_limit = 18'h0FFFC;
            5'd8: tile_limit = 18'h0FFF8;
            default: tile_limit = 18'h0FFF0;
        endcase
    logic tile_oob;
    always_comb tile_oob = tile_row_addr > tile_limit;

    /* A segment of the run is this tile's slice. An out of range tile
     * keeps the span as an immediate segment of zeros, so its columns are
     * transparent black. */
    logic [4:0] tile_px;
    always_comb tile_px = eff_w - {1'b0, tcol};
    always_comb begin
        mode2_win_wf = eff_w;
        mode2_win_hf = tile_h;
        mode2_sizeof_row = {5'd0, cf_width[14:0]};
        mode2_addr = state == S2_ADDR;
        mode2_data_row = q_row;
        mode2_seg_on = state == S2_SEG;
        mode2_run_ready = mstate == M_HAVE;
        mode2_run_max = {5'd0, tile_px};
        mode2_bpp = {1'b0, bpp_log};
    end
    always_comb begin
        mode2_seg_imm = !rm_run || tile_oob;
        mode2_seg_bits = mode2_seg_imm ? 23'd0
            : {4'd0, tile_row_addr[15:0], 3'b000}
              + (23'({19'd0, tcol}) << bpp_log);
    end

    initial begin
        state = S2_IDLE;
        mstate = M_IDLE;
        q_row = '0;
        r_row = '0;
        tile = '0;
        tcol = '0;
        tile_id = '0;
        div_q = '0;
        div_rem = '0;
        div_i = '0;
        div_den = '0;
        gnt_d1 = 1'b0;
        gnt_d = 1'b0;
        map_v = 1'b0;
        map_wq = '0;
        map_q = '0;
    end
    always_ff @(posedge clk) begin
        gnt_d1 <= a_gnt;
        gnt_d <= gnt_d1;
        if (abort_i) begin
`ifdef VERILATOR
            if (state != S2_IDLE && state != S2_SEG)
                $fatal(1, "mode2 underrun");
`endif
            state <= S2_IDLE;
            mstate <= M_IDLE;
        end else if (start) begin
            map_v <= 1'b0;
            state <= S2_WRAP;
        end else begin
            case (state)
                S2_IDLE: ;
                S2_WRAP:
                    if (rm_settle) begin
                        q_row <= tile16 ? {4'd0, rm_row[14:4]}
                                        : {3'd0, rm_row[14:3]};
                        r_row <= tile16 ? rm_row[3:0] : {1'b0, rm_row[2:0]};
                        div_q <= rm_row[14:0];
                        div_rem <= '0;
                        div_i <= '0;
                        div_den <= tile_h;
                        state <= rm_reject || y_trim == 4'd0
                            ? S2_ADDR : S2_DIVY;
                    end
                S2_DIVY: begin
                    div_q <= div_q_n;
                    div_rem <= div_rem_n;
                    div_i <= div_i + 4'd1;
                    if (div_i == 4'd14) begin
                        q_row <= div_q_n;
                        r_row <= div_rem_n[3:0];
                        state <= S2_ADDR;
                    end
                end
                S2_ADDR: begin
                    mstate <= M_IDLE;
                    if (rm_blank || rm_overrun || rm_col < 17'sd1) begin
                        tile <= '0;
                        tcol <= '0;
                        state <= S2_SEG;
                    end else if (x_trim == 4'd0) begin
                        tile <= tile16 ? {4'd0, rm_col[14:4]}
                                       : {3'd0, rm_col[14:3]};
                        tcol <= tile16 ? rm_col[3:0] : {1'b0, rm_col[2:0]};
                        state <= S2_SEG;
                    end else begin
                        div_q <= rm_col[14:0];
                        div_rem <= '0;
                        div_i <= '0;
                        div_den <= eff_w;
                        state <= S2_DIVX;
                    end
                end
                S2_DIVX: begin
                    div_q <= div_q_n;
                    div_rem <= div_rem_n;
                    div_i <= div_i + 4'd1;
                    if (div_i == 4'd14) begin
                        tile <= div_q_n;
                        tcol <= div_rem_n[3:0];
                        state <= S2_SEG;
                    end
                end
                S2_SEG: begin
                    case (mstate)
                        M_IDLE: if (!rm_right) begin
                            if (map_hit) begin
                                tile_id <= map_q[
                                    {map_addr[1:0], 3'b000}+:8];
                                mstate <= M_HAVE;
                            end else
                                mstate <= M_REQ;
                        end
                        M_REQ: if (a_gnt)
                            mstate <= M_WAIT;
                        M_WAIT: if (gnt_d) begin
                            tile_id <= a_rdata[
                                {map_addr[1:0], 3'b000}+:8];
                            map_q <= a_rdata;
                            map_wq <= map_addr[15:2];
                            map_v <= 1'b1;
                            mstate <= M_HAVE;
                        end
                        default: ;
                    endcase

                    if (rm_end)
                        state <= S2_IDLE;
                    if (seg_take && rm_run) begin
                        mstate <= M_IDLE;
                        tcol <= '0;
                        tile <= rm_wrap ? 15'd0 : tile + 15'd1;
                    end
                end
                default: state <= S2_IDLE;
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_mode2;
    always_comb unused_mode2 = ^{cfgw, cf_width[15], attr[15:12], attr[2],
                                     rm_row[16:15], div_rem[5:4],
                                     map_addr[16], tile_row_addr[17:16]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
