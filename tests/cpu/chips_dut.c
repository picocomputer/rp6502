/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator's CPU (vendor/chips w65c02.h, as overridden by
 * vendor/chips_rp6502) as a dut_t, so the suites in this directory can hold it
 * to the same evidence as the FPGA core's cpu65.sv.
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

const dut_t chips_dut = {
    .name = "chips w65c02",
    .reset = chips_reset,
    .begin = chips_begin,
    .bus = chips_bus,
    .tick = chips_tick,
    .end = chips_end,
};
