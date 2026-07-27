/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * SingleStepTests conformance for the emulator's CPU (vendor/chips w65c02.h,
 * as overridden by vendor/chips_rp6502). The FPGA core runs these same vectors
 * against cpu65.sv, which is what keeps the two implementations honest.
 *
 * Standalone: CHIPS_IMPL lives here, so this cannot link emu_core, which
 * carries its own copy and wires the CPU to the RP6502 bus rather than the
 * flat memory the vectors assume.
 */

#define CHIPS_IMPL
#include "chips/chips/w65c02.h"

#include "utest.h"
#include "vec.h"

static w65c02_t cpu;
static uint64_t pins;

static void chips_begin(const vec_regs_t *regs)
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

static void chips_bus(uint16_t *addr, bool *read)
{
    *addr = (uint16_t)(pins & 0xFFFF);
    *read = (pins & W65C02_RW) != 0;
}

static void chips_tick(uint8_t *data)
{
    if (pins & W65C02_RW)
        pins = (pins & ~0xFF0000ULL) | ((uint64_t)*data << 16);
    else
        *data = (uint8_t)((pins >> 16) & 0xFF);
    pins = w65c02_tick(&cpu, pins);
}

static void chips_end(vec_regs_t *regs)
{
    regs->pc = cpu.PC;
    regs->s = cpu.S;
    regs->a = cpu.A;
    regs->x = cpu.X;
    regs->y = cpu.Y;
    regs->p = cpu.P;
}

static const vec_cpu_t chips_cpu = {
    .name = "chips w65c02",
    .begin = chips_begin,
    .bus = chips_bus,
    .tick = chips_tick,
    .end = chips_end,
};

UTEST(w65c02, singlestep_vectors)
{
    vec_result_t r;
    ASSERT_TRUE(vec_run(VECTORS, &chips_cpu, -1, &r));
    if (r.failed)
        printf("%s\n", r.detail);
    ASSERT_EQ(r.failed, (size_t)0);
    ASSERT_GT(r.passed, (size_t)0);
}

/* The 16 bit-test branches are read-only and take 5, 6 or 7 cycles with the
 * branch. vendor/chips had them writing and always taking 6; see
 * vendor/chips_rp6502. */
UTEST(w65c02, bit_branches_never_write)
{
    vec_result_t r;
    for (int op = 0x0F; op <= 0xFF; op += 0x10)
    {
        ASSERT_TRUE(vec_run(VECTORS, &chips_cpu, op, &r));
        if (r.failed)
            printf("%s\n", r.detail);
        ASSERT_EQ(r.failed, (size_t)0);
        ASSERT_GT(r.passed, (size_t)0);
    }
}

UTEST_MAIN()
