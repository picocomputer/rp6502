/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The scanline program: the RTL image of the VGA side's prog table, minus
 * the function pointers — per scanline per plane, a fill slot and a
 * sprite slot. The fill slot is an enable/mode/attr word and the config
 * pointer; the sprite slot the same word shape, its second word packing
 * the sprite count over the config pointer. The soft CPU is the sole
 * author and every validation happens in its C, so the table is trusted
 * the way the fill functions trust their options.
 *
 * The table lives as four arrays, one per word of a slot pair, because
 * a block RAM has two ports and this table has four readers: the bus,
 * the walk's entry and config, and the sprite stage. Split by word each
 * array has exactly one writer and one reader, which is what the fabric
 * can build; kept whole it becomes a quarter of a million registers.
 * The soft CPU never reads the table back — vga.c only writes it — so
 * the bus side is write-only and the read port answers the registers
 * alone. The config word is sixteen bits wide because that is all a
 * config pointer is, which makes the split cost less memory than the
 * single table did.
 *
 * The canvas and the vsync line are shadows: the canvas latches where
 * the render takes the frame's first row, the vsync line at the beam's
 * own boundary. The vsync line is where the oracle counts the frame —
 * the highest programmed scanline, mid-frame once a mode programs fewer
 * lines than the raster — and it resets to 480 so the console machine
 * keeps its M3 cadence.
 *
 * Geometry decodes from the canvas per the VGA side's view table: 320-wide
 * canvases double each pixel, 240/180-line canvases double each line, and
 * the 180/360 canvases sit under a 60-line letterbox.
 */

module vid_prog (
    input logic clk,
    input logic rst_n,
    input logic frame_start,

    input logic [9:0] v,
    input logic px_first,
    output logic vid_prog_vsync_pulse,
    input logic [9:0] h,

    /* Latched geometry for the engines and the scanout. */
    output logic [2:0] vid_prog_canvas,
    output logic vid_prog_console,
    output logic vid_prog_x_shift,
    output logic vid_prog_y_shift,
    output logic [9:0] vid_prog_y_offset,

    /* Engine read port: one entry, registered. */
    input logic [8:0] p_line,
    input logic [1:0] p_plane,
    output logic [31:0] vid_prog_p_entry,
    output logic [15:0] vid_prog_p_config,

    /* Sprite stage read port: any word, registered. */
    input logic [12:0] s_idx,
    output logic [31:0] vid_prog_s_data,

    /* The sprite stage's lost-race count; a write clears it. */
    input logic [15:0] sp_overrun,
    output logic vid_prog_ov_clear,

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
    (* ramstyle = "no_rw_check" *)
    logic [31:0] spr_e[2048] /*verilator public_flat_rw*/;
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
                        spr_e[b_idx] <= b_wdata;
                    if (b_addr[3] && b_addr[2])
                        spr_c[b_idx] <= b_wdata;
                end
            end else begin
                vid_prog_b_rdata <= b_addr[3]
                    ? {16'd0, sp_overrun}
                    : (b_addr[2] ? {22'd0, vsync_shadow}
                                 : {29'd0, canvas_shadow});
            end
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            canvas_shadow <= 3'd0;
            vsync_shadow <= 10'd480;
            vsync_q <= 10'd480;
            vid_prog_canvas <= 3'd0;
        end else begin
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
    end

    always_comb vid_prog_ov_clear = b_stb && b_we && b_addr[15]
        && b_addr[3];

    always_comb begin
        vid_prog_console = vid_prog_canvas == 3'd0;
        vid_prog_x_shift = vid_prog_canvas == 3'd1 || vid_prog_canvas == 3'd2;
        vid_prog_y_shift = vid_prog_canvas == 3'd1 || vid_prog_canvas == 3'd2;
        vid_prog_y_offset = (vid_prog_canvas == 3'd2
                             || vid_prog_canvas == 3'd4) ? 10'd60 : 10'd0;
        vid_prog_vsync_pulse = h == 10'd0 && px_first && v == vsync_q;
    end

    /* The sprite stage only ever asks for words 2 and 3 — its index
     * carries a hard 1 in the word's high bit — so its two arrays
     * answer together and the low bit picks between them. */
    logic [31:0] s_e_q, s_c_q;
    logic s_half_q;
    always_ff @(posedge clk) begin
        vid_prog_p_entry <= fill_e[{p_line, p_plane}];
        vid_prog_p_config <= fill_c[{p_line, p_plane}];
        s_e_q <= spr_e[s_idx[12:2]];
        s_c_q <= spr_c[s_idx[12:2]];
        s_half_q <= s_idx[0];
    end
    always_comb vid_prog_s_data = s_half_q ? s_c_q : s_e_q;

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_prog;
    always_comb unused_vid_prog = ^{b_addr[1:0], b_wdata[31:16], s_idx[1]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
