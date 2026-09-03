/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's tightly coupled memory. A package rather than localparams
 * inside soc.sv because its port list names the address width, and a
 * parameter list cannot compute one -- Quartus rejects a localparam there.
 */

package tcm_pkg;

    localparam int TCM_WORDS = 24576;  // 96 KB
    localparam int TCM_AW = $clog2(TCM_WORDS);

endpackage
