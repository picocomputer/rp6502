/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mmio.h"

#include "ria/api/uni.h"

uint16_t uni_word(uint32_t index)
{
    uint32_t at = index * 2;
    return (uint16_t)OEMCP[at] | ((uint16_t)OEMCP[at + 1] << 8);
}
