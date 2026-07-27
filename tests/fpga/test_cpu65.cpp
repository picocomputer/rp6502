/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The suites in tests/cpu against the verilated cpu65. The emulator's CPU
 * answers the same dut_t in tests/cpu/chips_dut.c, so a divergence between the
 * two implementations fails here with the opcode and cycle named.
 *
 * State adoption for mid-stream vectors goes through verilator-public
 * registers; the reset path is the real one, no backdoor.
 */

#include "Vcpu65.h"
#include "Vcpu65___024root.h"

#include "dut.h"
#include "klaus.h"
#include "utest.h"
#include "vec.h"

#include <cstdio>

static Vcpu65 *dut;

static void clock_cycle()
{
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
}

static void rtl_reset(void)
{
    dut->rst_n = 0;
    clock_cycle();
    clock_cycle();
    dut->rst_n = 1;
}

static void rtl_begin(const dut_regs_t *regs)
{
    auto *r = dut->rootp;
    dut->rst_n = 1;
    r->cpu65__DOT__pc = regs->pc;
    r->cpu65__DOT__s = regs->s;
    r->cpu65__DOT__a = regs->a;
    r->cpu65__DOT__x = regs->x;
    r->cpu65__DOT__y = regs->y;
    r->cpu65__DOT__p = regs->p;
    r->cpu65__DOT__ad = 0;
    r->cpu65__DOT__ir = 0;
    r->cpu65__DOT__tick = 0;
    r->cpu65__DOT__irq_pip = 0;
    r->cpu65__DOT__nmi_pip = 0;
    r->cpu65__DOT__brk_irq = 0;
    r->cpu65__DOT__brk_nmi = 0;
    r->cpu65__DOT__brk_res = 0;
    r->cpu65__DOT__wait_flag = 0;
    r->cpu65__DOT__stop_flag = 0;
    r->cpu65__DOT__nmi_prev = 0;
    r->cpu65__DOT__res_seen = 0;
    /* Stand at the opcode fetch, the way the vectors start. */
    dut->cpu65_addr = regs->pc;
    dut->cpu65_we = 0;
    dut->cpu65_sync = 1;
}

static void rtl_bus(uint16_t *addr, bool *read, bool *sync)
{
    *addr = dut->cpu65_addr;
    *read = !dut->cpu65_we;
    *sync = dut->cpu65_sync;
}

static void rtl_tick(uint8_t *data)
{
    if (dut->cpu65_we)
        *data = dut->cpu65_data;
    else
        dut->data_i = *data;
    clock_cycle();
}

static void rtl_end(dut_regs_t *regs)
{
    auto *r = dut->rootp;
    regs->pc = r->cpu65__DOT__pc;
    regs->s = r->cpu65__DOT__s;
    regs->a = r->cpu65__DOT__a;
    regs->x = r->cpu65__DOT__x;
    regs->y = r->cpu65__DOT__y;
    regs->p = r->cpu65__DOT__p;
}

static const dut_t rtl_dut = {
    .name = "cpu65",
    .reset = rtl_reset,
    .begin = rtl_begin,
    .bus = rtl_bus,
    .tick = rtl_tick,
    .end = rtl_end,
};

UTEST(cpu65, singlestep_vectors)
{
    vec_result_t r;
    ASSERT_TRUE(vec_run(VECTORS, &rtl_dut, -1, &r));
    if (r.failed)
        printf("%s\n", r.detail);
    ASSERT_EQ(r.failed, (size_t)0);
    ASSERT_GT(r.passed, (size_t)0);
}

UTEST(cpu65, klaus_functional)
{
    klaus_result_t r;
    ASSERT_TRUE(klaus_run(KLAUS_6502, &rtl_dut, 500000000ull, &r));
    if (!r.trapped || r.trap_pc != 0x3469)
        printf("trapped=%d timed_out=%d pc=$%04X after %llu cycles\n",
               r.trapped, r.timed_out, r.trap_pc,
               (unsigned long long)r.cycles);
    ASSERT_FALSE(r.timed_out);
    ASSERT_EQ(r.trap_pc, 0x3469);
}

UTEST(cpu65, klaus_extended_opcodes)
{
    klaus_result_t r;
    ASSERT_TRUE(klaus_run(KLAUS_65C02, &rtl_dut, 500000000ull, &r));
    if (!r.trapped || r.trap_pc != 0x24F1)
        printf("trapped=%d timed_out=%d pc=$%04X after %llu cycles\n",
               r.trapped, r.timed_out, r.trap_pc,
               (unsigned long long)r.cycles);
    ASSERT_FALSE(r.timed_out);
    ASSERT_EQ(r.trap_pc, 0x24F1);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vcpu65;
    dut->en = 1;
    dut->irq_i = 0;
    dut->nmi_i = 0;
    dut->rdy_i = 0;
    dut->res_i = 0;
    rtl_reset();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
