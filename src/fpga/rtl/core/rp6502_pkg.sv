/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

package rp6502_pkg;

    // The RP6502 scans out 640x480@60 and nothing else, so a frame is always
    // 800x525 at a 25.2 MHz pixel clock. See emu/sys/vga.h.
    localparam int RP6502_V_TOTAL = 525;
    localparam int RP6502_SCANLINE_W = $clog2(RP6502_V_TOTAL);

endpackage
