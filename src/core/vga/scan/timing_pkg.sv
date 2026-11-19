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

    // A frame is always 800x525 pixels at a 25.2 MHz pixel clock, 60 Hz.
    localparam int H_TOTAL = 800;
    localparam int V_TOTAL = 525;
    localparam int SCANLINE_W = $clog2(V_TOTAL);

endpackage
