/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

package rp6502_pkg;

    // The RP6502 scans out 640x480@60 and nothing else, so a frame is always
    // 800x525 at a 25.2 MHz pixel clock. See emu/sys/vga.h; syncs are
    // active-low per the VGA side's scanvideo programming.
    localparam int RP6502_H_ACTIVE = 640;
    localparam int RP6502_H_FP = 16;
    localparam int RP6502_H_SYNC = 96;
    localparam int RP6502_H_TOTAL = 800;
    localparam int RP6502_V_ACTIVE = 480;
    localparam int RP6502_V_FP = 10;
    localparam int RP6502_V_SYNC = 2;
    localparam int RP6502_V_TOTAL = 525;
    localparam int RP6502_SCANLINE_W = $clog2(RP6502_V_TOTAL);

endpackage
