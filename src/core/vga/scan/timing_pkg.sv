/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A package rather than localparams inside timing.sv because SCANLINE_W
 * is derived from V_TOTAL and sets a port width in another module
 * (host/pocket/core/wiring.sv).
 */

package timing_pkg;

    // The machine scans out 640x480 at 60 Hz and nothing else, so a frame is
    // always 800x525 pixels at a 25.2 MHz pixel clock.
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
