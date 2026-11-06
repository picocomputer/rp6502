/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "psg_shim.h"

#include "core/aud/psg.h"
#include "core/aud/sine.h"

#include "aud_sine_tables.h"

int16_t sine_table[256];
uint16_t sine_pan[127];


void shim_init(void)
{
    for (int i = 0; i < 256; i++)
        sine_table[i] = AUD_SINE_TABLE[i];
    for (int i = 0; i < 127; i++)
        sine_pan[i] = AUD_PAN_TABLE[i];
}


/* A write found in the block, as the machines' mixers find it. */
void shim_xram_write(uint16_t addr, uint8_t val)
{
    const uint16_t at = psg_xaddr_get();
    if (at != 0xFFFF && (uint16_t)(addr - at) < PSG_REGS_SIZE)
        psg_put((uint16_t)(addr - at), val);
}
