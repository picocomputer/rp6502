/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The code page tables are in the staging store, and the staging window is
 * one byte wide, so each word of a table takes two reads.
 */

#include "mmio.h"

#include "core/str/unicode.h"

uint16_t unicode_word(uint32_t index)
{
    uint32_t at = index * 2;
    return (uint16_t)OEMCP[at] | ((uint16_t)OEMCP[at + 1] << 8);
}
