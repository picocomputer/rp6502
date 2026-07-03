/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#define CHIPS_IMPL
#include "m6522.h"
#include "emu/sys/mem.h"
#include "emu/sys/via.h"

static m6522_t via;

void via_reset(void)
{
    m6522_init(&via);
}

/* The live 6522 instance, for the debugger UI (ui_m6522). */
void *via_chip(void) { return &via; }

uint64_t via_tick(uint64_t pins)
{
    uint16_t addr = (uint16_t)(pins & 0xFFFFu);
    if (addr >= VIA_WINDOW_LO && addr <= VIA_WINDOW_HI)
        pins |= M6522_CS1; /* CS2 is held low, so CS1 high == selected */
    else
        pins &= ~M6522_CS1;
    return m6522_tick(&via, pins);
}
