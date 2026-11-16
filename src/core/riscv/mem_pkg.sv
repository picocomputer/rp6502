/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The soft CPU's memory is one cached space at address 0. The ROM block
 * is at its start: code and constants, loaded before reset and never
 * written. The RAM block follows it: data, heap and stack. The space is
 * a power of two; nothing is linked past the RAM block, so the soft CPU
 * reaches nothing else through the cache. */
package mem_pkg;

    localparam int MEM_WORDS = 131072;  // 512 KB
    localparam int MEM_AW = $clog2(MEM_WORDS);
    localparam int ROM_WORDS = 65536;  // 256 KB

endpackage
