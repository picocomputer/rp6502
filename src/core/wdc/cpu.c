/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define CHIPS_IMPL
#include "chips/chips/w65c02.h"
#include "core/wdc/cpu.h"

static w65c02_t cpu;

static uint64_t pins;

void (*cpu_dbg_cycle_cb)(uint64_t pins);

/* w65c02_tick switches on IR and post-increments it. Its cases stop at 0x7FF
 * and the default below them is marked unreachable, so a blob holding 0x7FF
 * with SYNC clear steps to 0x800 and the next tick has no case to land on. IR
 * at 0x7FF with SYNC set is harmless, because the tick reloads IR from the
 * data bus before it switches; neither field can be checked alone. */
static bool cpu_sst_valid(sst_cursor_t *c)
{
    if (cpu.IR > 0x7FF || (cpu.IR == 0x7FF && !(pins & W65C02_SYNC)))
        return sst_refuse(c, "a 6502 instruction step out of range");
    return true;
}

/* The list takes ROW, because X is a member of cpu. */
#define CPU_SST(ROW)                                            \
    ROW(U16, cpu.IR)                                            \
    ROW(U16, cpu.PC)                                            \
    ROW(U16, cpu.AD)                                            \
    ROW(U8, cpu.A)                                              \
    ROW(U8, cpu.X)                                              \
    ROW(U8, cpu.Y)                                              \
    ROW(U8, cpu.S)                                              \
    ROW(U8, cpu.P)                                              \
    ROW(U64, cpu.PINS)                                          \
    ROW(U16, cpu.irq_pip)                                       \
    ROW(U16, cpu.nmi_pip)                                       \
    ROW(U8, cpu.brk_flags)                                      \
    ROW(U8, cpu.wait_flag)                                      \
    ROW(U8, cpu.stop_flag)                                      \
    ROW(U64, pins)                                              \
    ROW(VALIDATE, cpu_sst_valid)                                \
    ROW(SKIP, cpu_dbg_cycle_cb, "the debugger's hook")
SST_TABLE(cpu, CPU_SST)

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
