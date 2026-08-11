/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

package rp6502_pkg;

    localparam int RP6502_H_ACTIVE = 640;
    localparam int RP6502_H_FP = 16;
    localparam int RP6502_H_SYNC = 96;
    localparam int RP6502_H_TOTAL = 800;
    localparam int RP6502_V_ACTIVE = 480;
    localparam int RP6502_V_FP = 10;
    localparam int RP6502_V_SYNC = 2;
    localparam int RP6502_V_TOTAL = 525;
    localparam int RP6502_SCANLINE_W = $clog2(RP6502_V_TOTAL);

    localparam int RP6502_TCM_WORDS = 24576;
    localparam int RP6502_TCM_AW = $clog2(RP6502_TCM_WORDS);

endpackage
