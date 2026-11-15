/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

package ram_pkg;

    localparam int RAM_WORDS = 65536;  // 256 KB
    localparam int RAM_AW = $clog2(RAM_WORDS);

endpackage
