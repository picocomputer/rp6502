/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator's CPU (vendor/chips w65c02.h) as a dut_t, so the suites in this
 * directory can hold it to the same evidence as the FPGA core's w65c02.sv.
 *
 * Standalone by necessity: CHIPS_IMPL lives here, so nothing that links this
 * can also link emu_core, which carries its own copy and wires the CPU to the
 * RP6502 bus rather than the flat memory the suites assume.
 */

#define CHIPS_IMPL
#include "chips/chips/w65c02.h"

#include "chips_dut.h"

static w65c02_t cpu;
static uint64_t pins;

/* Scripted pin levels, reapplied every tick. The model clears RES in its
 * returned mask when interrupt entry starts, but a pin is a level the
 * harness owns; holding it high must keep resetting, as it does in RTL.
 * Power-on reset is the exception: init plants RES for exactly one SYNC, so
 * it rides along as a one-shot rather than a level. */
static uint64_t pin_levels;
static uint64_t pin_once;

static void chips_begin(const dut_regs_t *regs)
{
    pins = w65c02_init(&cpu, &(w65c02_desc_t){0});
    cpu.PC = regs->pc;
    cpu.S = regs->s;
    cpu.A = regs->a;
    cpu.X = regs->x;
    cpu.Y = regs->y;
    cpu.P = regs->p;
    /* w65c02_init leaves RES pending; the vectors start mid-stream with the
     * CPU about to fetch an opcode, so enter at SYNC instead of a reset. */
    cpu.brk_flags = 0;
    cpu.irq_pip = 0;
    cpu.nmi_pip = 0;
    pins = W65C02_SYNC | W65C02_RW;
    pins = (pins & ~0xFFFFULL) | regs->pc;
}

static void chips_reset(void)
{
    pins = w65c02_init(&cpu, &(w65c02_desc_t){0});
    pin_once = W65C02_RES;
}

static void chips_bus(uint16_t *addr, bool *read, bool *sync)
{
    *addr = (uint16_t)(pins & 0xFFFF);
    *read = (pins & W65C02_RW) != 0;
    *sync = (pins & W65C02_SYNC) != 0;
}

static void chips_tick(uint8_t *data)
{
    if (pins & W65C02_RW)
        pins = (pins & ~0xFF0000ULL) | ((uint64_t)*data << 16);
    else
        *data = (uint8_t)((pins >> 16) & 0xFF);
    pins &= ~(W65C02_IRQ | W65C02_NMI | W65C02_RDY | W65C02_RES);
    pins |= pin_levels | pin_once;
    pin_once = 0;
    pins = w65c02_tick(&cpu, pins);
}

static void chips_end(dut_regs_t *regs)
{
    regs->pc = cpu.PC;
    regs->s = cpu.S;
    regs->a = cpu.A;
    regs->x = cpu.X;
    regs->y = cpu.Y;
    regs->p = cpu.P;
}

static void chips_pins(bool irq, bool nmi, bool rdy, bool res)
{
    pin_levels = 0;
    if (irq)
        pin_levels |= W65C02_IRQ;
    if (nmi)
        pin_levels |= W65C02_NMI;
    if (rdy)
        pin_levels |= W65C02_RDY;
    if (res)
        pin_levels |= W65C02_RES;
}

const dut_t chips_dut = {
    .name = "chips w65c02",
    .reset = chips_reset,
    .begin = chips_begin,
    .bus = chips_bus,
    .tick = chips_tick,
    .end = chips_end,
    .pins = chips_pins,
};
