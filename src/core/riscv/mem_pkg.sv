/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The soft CPU's memory is one cached space at address 0. The ROM block
 * is at its start: code and constants, loaded before reset and never
 * written. The RAM block follows it: data, heap and stack. The rest of
 * the space is spare. */
package mem_pkg;

    localparam int MEM_WORDS = 131072;  // 512 KB
    localparam int MEM_AW = $clog2(MEM_WORDS);
    localparam int ROM_WORDS = 65536;  // 256 KB
    localparam int RAM_WORDS = 16384;  // 64 KB

endpackage
