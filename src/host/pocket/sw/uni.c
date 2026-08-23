/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The code page tables ride in the staging store beside the fonts,
 * because five kilobytes is more than the TCM can spare.
 *
 * The staging window is byte-wide by construction, so a word is two
 * reads and a shift. That is why src/core/api/uni.c routes every table
 * access through this function instead of indexing an array.
 */

#include "mmio.h"

#include "core/api/uni.h"

uint16_t uni_word(uint32_t index)
{
    uint32_t at = index * 2;
    return (uint16_t)OEMCP[at] | ((uint16_t)OEMCP[at + 1] << 8);
}
