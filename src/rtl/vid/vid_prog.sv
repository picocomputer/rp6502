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
 * one writer and one reader, which is what the fabric can build; kept
 * whole it becomes a quarter of a million registers. The bus side is
 * write-only, since nothing reads the table back. The config word is
 * sixteen bits because that is all a config pointer is, which makes the
 * split cost less memory than the
 * single table did.
 *
 * The canvas and the vsync line are shadows: the canvas latches where
 * the render takes the frame's first row, the vsync line at the beam's
 * own boundary. The vsync line is where the oracle counts the frame —
 * the highest programmed scanline, mid-frame once a mode programs fewer
 * lines than the raster — and it resets to 480 so the console machine
 * keeps its M3 cadence.
 *
 * Geometry decodes from the canvas: a width and a height, which is all
 * a canvas is. The scaler owns presentation.
 */

module vid_prog (
    input logic clk,
    input logic frame_start,

    input logic [9:0] v,
    input logic px_first,
    output logic vid_prog_vsync_pulse,
    input logic [9:0] h,

    /* Latched geometry for the engines and the scanout. */
    output logic [2:0] vid_prog_canvas,
    output logic [9:0] vid_prog_cw,
    output logic [9:0] vid_prog_ch,

    /* Engine read port: one entry, registered. */
    input logic [8:0] p_line,
    input logic [1:0] p_plane,
    output logic [31:0] vid_prog_p_entry,
    output logic [15:0] vid_prog_p_config,

    /* Sprite stage read port: any word, registered. */
    input logic [12:0] s_idx,
    output logic [31:0] vid_prog_s_data,

    /* The render's lost races: the sprite stage's in the low half, the
     * planes' missed fills in the high. A write clears both. */

    /* The soft CPU: words 0-8191 the table at line*16 + plane*4 + word,
     * then bit 15 the registers — 0 canvas, 1 vsync line, 2 the
     * overrun count. */
    input logic b_stb,
    input logic b_we,
    input logic [15:0] b_addr,
    input logic [31:0] b_wdata,
    output logic [31:0] vid_prog_b_rdata
);

    /* Indexed by {line, plane}; the word within the pair picks the
     * array. w0/w1 are the fill slot, w2/w3 the sprite slot. */
    (* ramstyle = "no_rw_check" *)
    logic [31:0] fill_e[2048] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [15:0] fill_c[2048] /*verilator public_flat_rw*/;
    /* Twenty bits, not thirty-two. A sprite entry is an enable, a
     * three-bit mode and sixteen bits of attribute; vid_sprite marks
     * [30:19] unused and means it. Quartus trims fill_e to sixteen by
     * itself but cannot trim this one, because the read mux below ties
     * its width to spr_c's, and spr_c really does use all thirty-two.
     * Narrowing by hand is three M10K at a depth where a block holds
     * 10,240 bits: 2048x20 is four, 2048x32 is seven. */
    (* ramstyle = "no_rw_check" *)
    logic [19:0] spr_e[2048] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [31:0] spr_c[2048] /*verilator public_flat_rw*/;

    logic [10:0] b_idx;
    always_comb b_idx = b_addr[14:4];

    logic [2:0] canvas_shadow /*verilator public_flat_rd*/;
    logic [9:0] vsync_shadow /*verilator public_flat_rw*/;
    logic [9:0] vsync_q;

    always_ff @(posedge clk) begin
        if (b_stb) begin
            if (!b_addr[15]) begin
                vid_prog_b_rdata <= 32'd0;
                if (b_we) begin
                    if (!b_addr[3] && !b_addr[2])
                        fill_e[b_idx] <= b_wdata;
                    if (!b_addr[3] && b_addr[2])
                        fill_c[b_idx] <= b_wdata[15:0];
                    if (b_addr[3] && !b_addr[2])
                        spr_e[b_idx] <= {b_wdata[31], b_wdata[18:0]};
                    if (b_addr[3] && b_addr[2])
                        spr_c[b_idx] <= b_wdata;
                end
            end else begin
                vid_prog_b_rdata <= b_addr[3]
                    ? 32'd0
                    : (b_addr[2] ? {22'd0, vsync_shadow}
                                 : {29'd0, canvas_shadow});
            end
        end
    end

    initial begin
        canvas_shadow = 3'd0;
        vsync_shadow = 10'd480;
        vsync_q = 10'd480;
        vid_prog_canvas = 3'd0;
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
            vid_prog_canvas <= canvas_shadow;
        if (frame_start)
            vsync_q <= vsync_shadow;
    end


    always_comb begin
        vid_prog_cw = (vid_prog_canvas == 3'd1 || vid_prog_canvas == 3'd2)
            ? 10'd320 : 10'd640;
        vid_prog_ch = vid_prog_canvas == 3'd1 ? 10'd240
            : vid_prog_canvas == 3'd2 ? 10'd180
            : vid_prog_canvas == 3'd4 ? 10'd360 : 10'd480;
        vid_prog_vsync_pulse = h == 10'd0 && px_first && v == vsync_q;
    end

    /* The sprite stage only ever asks for words 2 and 3 — its index
     * carries a hard 1 in the word's high bit — so its two arrays
     * answer together and the low bit picks between them. */
    logic [19:0] s_e_q;
    logic [31:0] s_c_q;
    logic s_half_q;
    always_ff @(posedge clk) begin
        vid_prog_p_entry <= fill_e[{p_line, p_plane}];
        vid_prog_p_config <= fill_c[{p_line, p_plane}];
        s_e_q <= spr_e[s_idx[12:2]];
        s_c_q <= spr_c[s_idx[12:2]];
        s_half_q <= s_idx[0];
    end
    always_comb vid_prog_s_data =
        s_half_q ? s_c_q : {s_e_q[19], 12'd0, s_e_q[18:0]};

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_prog;
    always_comb unused_vid_prog = ^{b_addr[1:0], b_wdata[31:16], s_idx[1]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
