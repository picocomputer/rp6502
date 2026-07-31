/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Where this machine keeps its code page tables. Five kilobytes will
 * not fit in a 48 KB tightly coupled memory that already holds the
 * firmware, so they ride in the staging store beside the fonts, loaded
 * from oemcp.bin the same way and by the same data slot machinery.
 *
 * The staging window is byte-wide by construction — a read there puts
 * one byte on every lane — so a word is two reads and a shift. That is
 * the whole reason src/ria/api/uni.c routes every table access through
 * this function instead of indexing an array.
 */

#include "mmio.h"

#include "ria/api/uni.h"

uint16_t uni_word(uint32_t index)
{
    uint32_t at = index * 2;
    return (uint16_t)OEMCP[at] | ((uint16_t)OEMCP[at + 1] << 8);
}
