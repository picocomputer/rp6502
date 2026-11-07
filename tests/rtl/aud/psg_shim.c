/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "psg_shim.h"

#include "core/aud/psg.h"

/* A write found in the block, as the machines' mixers find it. */
void shim_xram_write(uint16_t addr, uint8_t val)
{
    const uint16_t at = psg_xaddr_get();
    if (at != 0xFFFF && (uint16_t)(addr - at) < PSG_REGS_SIZE)
        psg_put((uint16_t)(addr - at), val);
}
