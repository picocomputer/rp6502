/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The keyboard layouts out of the staging store, one word at a time.
 * Same trade as uni.c: twenty kilobytes has no room in the TCM, and the
 * window cannot fetch anything wider than a byte, so every access goes
 * through this one function.
 */

#include "mmio.h"

#include "core/hid/layout.h"

uint16_t layout_word(uint32_t index)
{
    uint32_t at = index * 2;
    return (uint16_t)KBDLAY[at] | ((uint16_t)KBDLAY[at + 1] << 8);
}
