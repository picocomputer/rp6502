/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

package tcm_pkg;

    localparam int TCM_WORDS = 24576;  // 96 KB
    localparam int TCM_AW = $clog2(TCM_WORDS);

endpackage
