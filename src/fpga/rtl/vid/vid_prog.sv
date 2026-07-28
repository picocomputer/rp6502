/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The scanline program: the RTL image of the VGA side's prog table, minus
 * the function pointers — per scanline per plane, an enable, the mode, the
 * attribute word, and the config pointer into XRAM. The soft CPU is the
 * sole author and every validation happens in its C, so the table is
 * trusted the way the fill functions trust their options.
 *
 * The canvas and the vsync line are shadows latched at frame start. The
 * vsync line is where the oracle counts the frame — the highest programmed
 * scanline, mid-frame once a mode programs fewer lines than the raster —
 * and it resets to 480 so the console machine keeps its M3 cadence.
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

    /* The soft CPU: words 0-4095 the table at line*8 + plane*2 + word,
     * then bit 14 the registers — 0 canvas, 1 vsync line. */
    input logic b_stb,
    input logic b_we,
    input logic [14:0] b_addr,
    input logic [31:0] b_wdata,
    output logic [31:0] vid_prog_b_rdata
);

    logic [31:0] prog[4096] /*verilator public_flat_rw*/;

    logic [2:0] canvas_shadow /*verilator public_flat_rd*/;
    logic [9:0] vsync_shadow /*verilator public_flat_rw*/;
    logic [9:0] vsync_q;

    always_ff @(posedge clk) begin
        if (b_stb) begin
            if (!b_addr[14]) begin
                vid_prog_b_rdata <= prog[b_addr[13:2]];
                if (b_we)
                    prog[b_addr[13:2]] <= b_wdata;
            end else begin
                vid_prog_b_rdata <= b_addr[2]
                    ? {22'd0, vsync_shadow} : {29'd0, canvas_shadow};
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
            if (b_stb && b_we && b_addr[14]) begin
                if (b_addr[2])
                    vsync_shadow <= b_wdata[9:0];
                else
                    canvas_shadow <= b_wdata[2:0];
            end
            if (frame_start) begin
                vsync_q <= vsync_shadow;
                vid_prog_canvas <= canvas_shadow;
            end
        end
    end

    always_comb begin
        vid_prog_console = vid_prog_canvas == 3'd0;
        vid_prog_x_shift = vid_prog_canvas == 3'd1 || vid_prog_canvas == 3'd2;
        vid_prog_y_shift = vid_prog_canvas == 3'd1 || vid_prog_canvas == 3'd2;
        vid_prog_y_offset = (vid_prog_canvas == 3'd2
                             || vid_prog_canvas == 3'd4) ? 10'd60 : 10'd0;
        vid_prog_vsync_pulse = h == 10'd0 && v == vsync_q;
    end

    always_ff @(posedge clk) begin
        vid_prog_p_entry <= prog[{p_line, p_plane, 1'b0}];
        vid_prog_p_config <= prog[{p_line, p_plane, 1'b1}][15:0];
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid_prog;
    always_comb unused_vid_prog = ^{b_addr[1:0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
