/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The keyboard layouts out of the staging store, one word at a time.
 *
 * Same trade as uni.c beside it: twenty kilobytes of table has no room
 * in a 96 KB tightly coupled memory, so the layouts ride in on their
 * own data slot and are read through a window that cannot fetch
 * anything wider than a byte. That only works if every access goes
 * through one function, which is why there is one.
 */

#include "mmio.h"

#include "ria/hid/kbl.h"

uint16_t kbl_word(uint32_t index)
{
    uint32_t at = index * 2;
    return (uint16_t)KBDLAY[at] | ((uint16_t)KBDLAY[at + 1] << 8);
}
