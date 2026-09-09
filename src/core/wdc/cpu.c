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

void cpu_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u16(c, cpu.IR);
    sst_put_u16(c, cpu.PC);
    sst_put_u16(c, cpu.AD);
    sst_put_u8(c, cpu.A);
    sst_put_u8(c, cpu.X);
    sst_put_u8(c, cpu.Y);
    sst_put_u8(c, cpu.S);
    sst_put_u8(c, cpu.P);
    sst_put_u64(c, cpu.PINS);
    sst_put_u16(c, cpu.irq_pip);
    sst_put_u16(c, cpu.nmi_pip);
    sst_put_u8(c, cpu.brk_flags);
    sst_put_u8(c, cpu.wait_flag);
    sst_put_u8(c, cpu.stop_flag);
    sst_put_u64(c, pins);
}

bool cpu_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    w65c02_t in;
    in.IR = sst_get_u16(c);
    in.PC = sst_get_u16(c);
    in.AD = sst_get_u16(c);
    in.A = sst_get_u8(c);
    in.X = sst_get_u8(c);
    in.Y = sst_get_u8(c);
    in.S = sst_get_u8(c);
    in.P = sst_get_u8(c);
    in.PINS = sst_get_u64(c);
    in.irq_pip = sst_get_u16(c);
    in.nmi_pip = sst_get_u16(c);
    in.brk_flags = sst_get_u8(c);
    in.wait_flag = sst_get_u8(c);
    in.stop_flag = sst_get_u8(c);
    uint64_t board = sst_get_u64(c);
    if (!sst_ok(c))
        return false;
    /* w65c02_tick switches on IR and post-increments it. Its cases stop at
     * 0x7FF and the default below them is marked unreachable, so a blob
     * holding 0x7FF with SYNC clear steps to 0x800 and the next tick has no
     * case to land on. IR at 0x7FF with SYNC set is harmless, because the tick
     * reloads IR from the data bus before it switches; neither field can be
     * checked alone. */
    if (in.IR > 0x7FF || (in.IR == 0x7FF && !(board & W65C02_SYNC)))
        return false;
    cpu = in;
    pins = board;
    return true;
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
