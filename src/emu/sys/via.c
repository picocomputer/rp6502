/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6522 VIA — a physical timer/IO chip on the RP6502 bus at $FFD0-$FFDF.
 * The RIA doesn't model it (it's real silicon beside the 6502), so the emulator
 * provides it from floooh's m6522. Programs use Timer 1 in free-run mode to
 * raise a periodic IRQ (e.g. paint's 125 Hz mouse poll); the VIA's IRQ pin
 * shares M6502_IRQ's bit position, so it drives the CPU's IRQ line directly.
 *
 * m6522's pin layout overlaps the 6502's by design — D0-D7, RW and IRQ sit at
 * the same bit positions and RS0-3 are the low address nibble — so a VIA tick
 * reads everything it needs straight from the CPU pin mask; we only add the
 * chip select for accesses that land in its window.
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
