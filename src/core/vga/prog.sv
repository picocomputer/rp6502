/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The scanline program: per scanline per plane, a fill slot and a
 * sprite slot. The soft CPU is the sole author and every validation
 * happens in its C, so the table is trusted.
 *
 * Four arrays, one per word of a slot pair, because a block RAM has two
 * ports and this table has four readers. Split by word each array has
 * one render reader, which fits a block RAM; kept whole it becomes a
 * quarter of a million registers.
 *
 * The render reads on the machine's clock, so a savestate's stop leaves
 * an answer in flight where it was. The soft CPU writes, and the
 * savestate engine reads and writes, through each array's other port,
 * which keeps its clock while the machine is stopped. At this depth a
 * block with two such ports costs no more blocks than one with a single
 * reader.
 *
 * The canvas and the vsync line are shadows: the canvas latches where
 * the render takes the frame's first row, the vsync line at the beam's
 * own boundary. The vsync line is where the oracle counts the frame —
 * the highest programmed scanline, mid-frame once a mode programs fewer
 * lines than the raster — and it resets to 480 so the console machine
 * keeps its M3 cadence.
 *
 * Geometry decodes from the canvas: a width and a height, which is all
 * a canvas is. The Pocket's scaler handles presentation.
 */

module prog (
    input logic clk,
    /* The table runs on the clock that does not stop. */
    input logic clk_mem,
    input logic frame_start,

    input logic [9:0] v,
    input logic px_first,
    output logic prog_vsync_pulse,
    input logic [9:0] h,

    output logic [2:0] prog_canvas,
    output logic [9:0] prog_cw,
    output logic [9:0] prog_ch,

    input logic [8:0] p_line,
    input logic [1:0] p_plane,
    output logic [31:0] prog_p_entry,
    output logic [15:0] prog_p_config,

    input logic [12:0] s_idx,
    output logic [31:0] prog_s_data,

    /* The savestate serializer, with the machine stopped. Word 0 and 1
     * are the fill slot, 2 and 3 the sprite slot. */
    input logic sst_own,
    input logic [10:0] sst_addr,
    input logic [1:0] sst_word,
    input logic sst_we,
    input logic [31:0] sst_wdata,
    output logic [31:0] prog_sst_rdata,

    /* The soft CPU: words 0-8191 the table at line*16 + plane*4 + word,
     * then bit 15 the registers — 0 canvas, 1 vsync line. */
    input logic b_stb,
    input logic b_we,
    input logic [15:0] b_addr,
    input logic [31:0] b_wdata
);

    /* Indexed by {line, plane}; the word within the pair picks the
     * array. w0/w1 are the fill slot, w2/w3 the sprite slot. */
    (* ramstyle = "no_rw_check" *)
    logic [31:0] fill_e[2048] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [15:0] fill_c[2048] /*verilator public_flat_rw*/;
    /* Twenty bits, not thirty-two: a sprite entry is an enable, a
     * three-bit mode and sixteen bits of attribute. Quartus cannot trim
     * this one itself, because the read mux below ties its width to
     * spr_c's, and spr_c does use all thirty-two. Narrowing by hand is
     * three M10K — 2048x20 is four blocks, 2048x32 is seven. */
    (* ramstyle = "no_rw_check" *)
    logic [19:0] spr_e[2048] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [31:0] spr_c[2048] /*verilator public_flat_rw*/;

    logic [10:0] b_idx;
    always_comb b_idx = b_addr[14:4];

    logic [2:0] canvas_shadow /*verilator public_flat_rd*/;
    logic [9:0] vsync_shadow /*verilator public_flat_rw*/;
    logic [9:0] vsync_q;

    /* One write port per array, whoever is writing: a block RAM has
     * one, and a second conditional write -- even one that can never
     * fire at the same time -- is a second port, which un-infers the
     * RAM and builds the table out of registers. Two hundred kilobits
     * of registers is eight of these chips. */
    logic [10:0] w_a;
    logic [1:0] w_word;
    logic [31:0] w_d;
    logic w_go;
    always_comb begin
        w_a = sst_own ? sst_addr : b_idx;
        w_word = sst_own ? sst_word : b_addr[3:2];
        w_d = sst_own ? sst_wdata : b_wdata;
        w_go = sst_own ? sst_we : (b_stb && !b_addr[15] && b_we);
    end
    logic [31:0] fe_b, sc_b;
    logic [15:0] fc_b;
    logic [19:0] se_b;
    always_ff @(posedge clk_mem) begin
        if (w_go && w_word == 2'd0) fill_e[w_a] <= w_d;
        fe_b <= fill_e[w_a];
    end
    always_ff @(posedge clk_mem) begin
        if (w_go && w_word == 2'd1) fill_c[w_a] <= w_d[15:0];
        fc_b <= fill_c[w_a];
    end
    always_ff @(posedge clk_mem) begin
        if (w_go && w_word == 2'd2) spr_e[w_a] <= {w_d[31], w_d[18:0]};
        se_b <= spr_e[w_a];
    end
    always_ff @(posedge clk_mem) begin
        if (w_go && w_word == 2'd3) spr_c[w_a] <= w_d;
        sc_b <= spr_c[w_a];
    end

    logic [19:0] s_e_q;
    logic [31:0] s_c_q;
    logic s_half_q;

    always_comb begin
        case (sst_word)
            2'd0: prog_sst_rdata = fe_b;
            2'd1: prog_sst_rdata = {16'd0, fc_b};
            2'd2: prog_sst_rdata = {se_b[19], 12'd0, se_b[18:0]};
            default: prog_sst_rdata = sc_b;
        endcase
    end

    initial begin
        canvas_shadow = 3'd0;
        vsync_shadow = 10'd480;
        vsync_q = 10'd480;
        prog_canvas = 3'd0;
    end
    always_ff @(posedge clk) begin
        if (b_stb && b_we && b_addr[15] && !b_addr[3]) begin
            if (b_addr[2])
                vsync_shadow <= b_wdata[9:0];
            else
                canvas_shadow <= b_wdata[2:0];
        end
        /* The canvas is a render latch: it fires one raster line
         * before the beam's frame, where the engines take row 0, so
         * row 0 sees the same canvas as every other row. The vsync
         * line paces the beam and latches at the beam's boundary. */
        if (h == 10'd0 && px_first && v == 10'd524)
            prog_canvas <= canvas_shadow;
        if (frame_start)
            vsync_q <= vsync_shadow;
    end


    always_comb begin
        prog_cw = (prog_canvas == 3'd1 || prog_canvas == 3'd2)
            ? 10'd320 : 10'd640;
        prog_ch = prog_canvas == 3'd1 ? 10'd240
            : prog_canvas == 3'd2 ? 10'd180
            : prog_canvas == 3'd4 ? 10'd360 : 10'd480;
        prog_vsync_pulse = h == 10'd0 && px_first && v == vsync_q;
    end

    /* The sprite stage only ever asks for words 2 and 3 — its index
     * carries a hard 1 in the word's high bit — so its two arrays
     * answer together and the low bit picks between them. */
    always_ff @(posedge clk) begin
        prog_p_entry <= fill_e[{p_line, p_plane}];
        prog_p_config <= fill_c[{p_line, p_plane}];
        s_e_q <= spr_e[s_idx[12:2]];
        s_c_q <= spr_c[s_idx[12:2]];
        s_half_q <= s_idx[0];
    end
    always_comb prog_s_data =
        s_half_q ? s_c_q : {s_e_q[19], 12'd0, s_e_q[18:0]};

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_prog;
    always_comb unused_prog = ^{b_addr[1:0], b_wdata[31:16], s_idx[1]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
