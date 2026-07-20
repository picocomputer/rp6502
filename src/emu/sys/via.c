/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

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

bool via_tick(uint16_t addr, bool read, uint8_t *data)
{
    /* The VIA's own pins, built fresh from the bus each cycle (the vendor pattern:
     * see _vic20_tick). PA/PB/CA/CB are deliberately left clear — nothing is wired to
     * this VIA's ports, so its inputs read low. Carrying them across cycles instead
     * would feed the chip its own driven outputs back as inputs, latching a bit high
     * forever once DDR flips it to input. */
    const bool selected = addr >= VIA_MMAP_LO && addr <= VIA_MMAP_HI;
    uint64_t pins = addr & M6522_RS_PINS; /* A0-A3 are the register select */
    if (read)
        pins |= M6522_RW;
    else
        M6522_SET_DATA(pins, *data);
    if (selected)
        pins |= M6522_CS1; /* CS2 is held low, so CS1 high == selected */

    pins = m6522_tick(&via, pins);

    if (selected && read)
        *data = M6522_GET_DATA(pins);
    return (pins & M6522_IRQ) != 0;
}
