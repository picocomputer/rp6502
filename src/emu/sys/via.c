/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/chips/w65c02.h" /* M6502_* pin macros; CHIPS_IMPL is in sys/cpu.c */

#define CHIPS_IMPL
#include "m6522.h"
#include "emu/sys/via.h"

static m6522_t via;

/* Program start: reset the VIA. The VIA shares the 6502 RESB, so this runs just
 * before cpu_run in the run fan-out. */
void via_run(void)
{
    m6522_init(&via);
}

/* The live 6522 instance, for the debugger UI. */
void *via_chip(void) { return &via; }

uint64_t via_tick(uint64_t pins, bool selected)
{
    /* The VIA's own pins, built fresh from the bus each cycle (the vendor pattern:
     * see _vic20_tick). PA/PB/CA/CB sit above M6502_PIN_MASK and are deliberately
     * left clear — nothing is wired to this VIA's ports, so its inputs read low.
     * Threading one shared mask instead would feed the chip its own driven outputs
     * back as inputs, latching a bit high forever once DDR flips it to input. */
    uint64_t via_pins = pins & M6502_PIN_MASK;
    if (selected)
        via_pins |= M6522_CS1; /* CS2 is held low, so CS1 high == selected */
    return m6522_tick(&via, via_pins);
}
