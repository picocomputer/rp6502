/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mmio.h"

#include "ria/hid/kbl.h"

uint16_t kbl_word(uint32_t index)
{
    uint32_t at = index * 2;
    return (uint16_t)KBDLAY[at] | ((uint16_t)KBDLAY[at + 1] << 8);
}
