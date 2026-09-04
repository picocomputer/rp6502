/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The raster, as numbers. A package rather than localparams inside timing.sv
 * because the scanline counter's width is derived from V_TOTAL and names a
 * port, and a parameter list cannot compute one -- Quartus rejects a
 * localparam there.
 */

package timing_pkg;

    // The machine scans out 640x480@60 and nothing else, so a frame is always
    // 800x525 at a 25.2 MHz pixel clock. See core/vga/vga.h; syncs are
    // active-low per the VGA side's scanvideo programming.
    localparam int H_ACTIVE = 640;
    localparam int H_FP = 16;
    localparam int H_SYNC = 96;
    localparam int H_TOTAL = 800;
    localparam int V_ACTIVE = 480;
    localparam int V_FP = 10;
    localparam int V_SYNC = 2;
    localparam int V_TOTAL = 525;
    localparam int SCANLINE_W = $clog2(V_TOTAL);

endpackage
