/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The vendored model, and the board's side of it.
 */

#define CHIPS_IMPL
#include "chips/chips/w65c02.h"
#include "core/wdc/cpu.h"

static w65c02_t cpu;

/* The 6502 bus, in the w65c02's own pin layout. Private to this file -- the
 * board speaks decoded signals. */
static uint64_t pins;

/* Display-only per-cycle observer for the on-screen ui_dbg view. The window
 * overlay registers dbgui_tick here; NULL otherwise, so the hot tick loop pays
 * only a null check. It MUST NOT gate the CPU -- dbg.c is the one
 * authoritative engine. */
void (*cpu_dbg_cycle_cb)(uint64_t pins);

void *cpu_chip(void) { return &cpu; }

void cpu_reset(void)
{
    pins = w65c02_init(&cpu, &(w65c02_desc_t){0});
}

void cpu_tick(uint16_t *addr, bool *read, uint8_t *data, bool irq)
{
    if (irq)
        pins |= W65C02_IRQ;
    else
        pins &= ~W65C02_IRQ;
    W65C02_SET_DATA(pins, *data);

    pins = w65c02_tick(&cpu, pins);

    *addr = W65C02_GET_ADDR(pins);
    *read = (pins & W65C02_RW) != 0;
    *data = W65C02_GET_DATA(pins);
}

uint64_t cpu_dbg_pins(void) { return pins; }

bool cpu_opcode_fetch(uint16_t *pc, uint8_t *sp)
{
    if (!(pins & W65C02_SYNC))
        return false;
    *pc = W65C02_GET_ADDR(pins);
    *sp = w65c02_s(&cpu);
    return true;
}
