/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cpu65_dut.h"

#include "Vcpu65.h"
#include "Vcpu65___024root.h"

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

static void rtl_pins(bool irq, bool nmi, bool rdy, bool res)
{
    dut->irq_i = irq;
    dut->nmi_i = nmi;
    dut->rdy_i = rdy;
    dut->res_i = res;
}

const dut_t cpu65_dut = {
    .name = "cpu65",
    .reset = rtl_reset,
    .begin = rtl_begin,
    .bus = rtl_bus,
    .tick = rtl_tick,
    .end = rtl_end,
    .pins = rtl_pins,
};

void cpu65_dut_init(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vcpu65;
    dut->en = 1;
    dut->irq_i = 0;
    dut->nmi_i = 0;
    dut->rdy_i = 0;
    dut->res_i = 0;
    rtl_reset();
}

void cpu65_dut_free(void)
{
    dut->final();
    delete dut;
    dut = nullptr;
}
