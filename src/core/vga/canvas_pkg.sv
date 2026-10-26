/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The canvases, as core/vga/vga.h lists them for the C: a code, and the
 * width and height it names. prog.sv decodes the canvas the machine
 * draws and the Pocket's pocket_video.sv decodes the canvas it hands to
 * the scaler. If the two disagreed, the reader would pop a different
 * number of pixels than the machine pushed, so the list is kept once,
 * here.
 *
 * The console is 640x480. vga.h lets a host give the console another
 * size, but the Pocket never does, so the console (0) and 640x480 (3)
 * canvases decode alike, as the default, and need no names here.
 */

package canvas_pkg;

    /* These are vga.h's vga_canvas_t values. */
    localparam logic [2:0] CV_320_240 = 3'd1;
    localparam logic [2:0] CV_320_180 = 3'd2;
    localparam logic [2:0] CV_640_360 = 3'd4;
    localparam logic [2:0] CV_320_200 = 3'd5;
    localparam logic [2:0] CV_640_400 = 3'd6;

    function automatic logic [9:0] canvas_w(input logic [2:0] cv);
        case (cv)
            CV_320_240, CV_320_180, CV_320_200: canvas_w = 10'd320;
            default: canvas_w = 10'd640;
        endcase
    endfunction

    function automatic logic [9:0] canvas_h(input logic [2:0] cv);
        case (cv)
            CV_320_240: canvas_h = 10'd240;
            CV_320_180: canvas_h = 10'd180;
            CV_640_360: canvas_h = 10'd360;
            CV_320_200: canvas_h = 10'd200;
            CV_640_400: canvas_h = 10'd400;
            default: canvas_h = 10'd480;
        endcase
    endfunction

endpackage
