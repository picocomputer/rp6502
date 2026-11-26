/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 6, the pointer, the fabric's copy of what core/vga/mode/mode6.c
 * keeps: the shape and where its hot spot is, the program's pointers in
 * XRAM, and the system's shape. scan/sprite.sv draws it, with mode 5's
 * engine, as a sprite over every plane.
 *
 * The soft CPU writes these and reads nothing back; host/pocket/sw/vga.c
 * keeps its own copy. At 0x50200000:
 *   word 0 shape {copy[8], shape[7:0]}, 0 for none;
 *   word 1 where {y[31:16], x[15:0]}, the hot spot in canvas pixels;
 *   word 2 config, the program's sprite array in XRAM, $FFFF for the
 *      system's shape;
 *   word 3 and 4 the system shape in copy 0 and 1
 *      {width_height[23:16], hot_y[15:8], hot_x[7:0]}.
 * From 0x50201000 the system shape's image, a word at a time: 128 words
 * of copy 0, then 128 of copy 1. The firmware fills the copy that is not
 * showing and then names it in the shape word.
 *
 * The shape, where and config latch together as a frame starts, on the
 * line row 0 is drawn on, so every row of a frame draws one pointer and
 * the copy a frame draws is never the one being written.
 */

module mode6 (
    input logic clk,
    input logic frame_start,

    input logic b_stb,
    input logic b_we,
    input logic [12:0] b_addr,
    input logic [31:0] b_wdata,

    /* What this frame draws: whether a pointer is on, whether it is the
     * system's, and the address of a program's descriptor. */
    output logic mode6_on,
    output logic mode6_sys,
    output logic [15:0] mode6_at,
    output logic signed [15:0] mode6_x,
    output logic signed [15:0] mode6_y,
    output logic [7:0] mode6_hot_x,
    output logic [7:0] mode6_hot_y,
    output logic [7:0] mode6_wh,

    /* The system shape's image, read by word: the word asked for on a
     * clock is on mode6_q two clocks on, as XRAM's would be. */
    input logic [6:0] img_addr,
    output logic [31:0] mode6_q
);

    logic [8:0] shape_s;
    logic [31:0] where_s;
    logic [15:0] config_s;
    logic [23:0] sys_s[2];

    /* Both copies of the image in one block. */
    (* ramstyle = "M10K, no_rw_check" *)
    logic [31:0] img[256] /*verilator public_flat_rw*/;

    logic reg_w, img_w;
    always_comb begin
        reg_w = b_stb && b_we && !b_addr[12];
        img_w = b_stb && b_we && b_addr[12];
    end

    initial begin
        shape_s = '0;
        where_s = '0;
        config_s = 16'hFFFF;
        sys_s[0] = '0;
        sys_s[1] = '0;
    end
    always_ff @(posedge clk) begin
        if (reg_w)
            case (b_addr[4:2])
                3'd0: shape_s <= b_wdata[8:0];
                3'd1: where_s <= b_wdata;
                3'd2: config_s <= b_wdata[15:0];
                3'd3: sys_s[0] <= b_wdata[23:0];
                3'd4: sys_s[1] <= b_wdata[23:0];
                default: ;
            endcase
    end
    always_ff @(posedge clk)
        if (img_w)
            img[b_addr[9:2]] <= b_wdata;

    /* A program's pointer n is the descriptor n-1 of its array, ten
     * bytes, and draws nothing when that runs past XRAM, as mode6.c
     * checks. */
    logic [7:0] shape_q;
    logic copy_q;
    logic [15:0] config_q;
    logic [16:0] at;
    always_comb at = {1'b0, config_q}
        + {6'd0, shape_q - 8'd1, 3'd0} + {8'd0, shape_q - 8'd1, 1'b0};
    initial begin
        shape_q = '0;
        copy_q = 1'b0;
        config_q = 16'hFFFF;
        mode6_x = '0;
        mode6_y = '0;
    end
    always_ff @(posedge clk)
        if (frame_start) begin
            shape_q <= shape_s[7:0];
            copy_q <= shape_s[8];
            config_q <= config_s;
            mode6_x <= where_s[15:0];
            mode6_y <= where_s[31:16];
        end

    always_comb begin
        mode6_sys = config_q == 16'hFFFF;
        mode6_at = at[15:0];
        mode6_on = shape_q != 8'd0
            && (mode6_sys || at <= 17'h10000 - 17'd10);
        {mode6_wh, mode6_hot_y, mode6_hot_x} = sys_s[copy_q];
    end

    logic [7:0] img_ra;
    always_ff @(posedge clk) begin
        img_ra <= {copy_q, img_addr};
        mode6_q <= img[img_ra];
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_mode6;
    always_comb unused_mode6 = ^{b_addr[11:10], b_addr[1:0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
