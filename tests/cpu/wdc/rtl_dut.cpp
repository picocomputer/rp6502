/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtl_dut.h"

#include "Vcpu.h"
#include "Vcpu___024root.h"

static Vcpu *dut;

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
    r->cpu__DOT__pc = regs->pc;
    r->cpu__DOT__s = regs->s;
    r->cpu__DOT__a = regs->a;
    r->cpu__DOT__x = regs->x;
    r->cpu__DOT__y = regs->y;
    r->cpu__DOT__p = regs->p;
    r->cpu__DOT__ad = 0;
    r->cpu__DOT__ir = 0;
    r->cpu__DOT__tick = 0;
    r->cpu__DOT__irq_pip = 0;
    r->cpu__DOT__nmi_pip = 0;
    r->cpu__DOT__brk_irq = 0;
    r->cpu__DOT__brk_nmi = 0;
    r->cpu__DOT__brk_res = 0;
    r->cpu__DOT__wait_flag = 0;
    r->cpu__DOT__stop_flag = 0;
    r->cpu__DOT__nmi_prev = 0;
    r->cpu__DOT__res_seen = 0;
    /* Stand at the opcode fetch, the way the vectors start. */
    dut->cpu_addr = regs->pc;
    dut->cpu_we = 0;
    r->cpu__DOT__cpu_sync = 1;
}

static void rtl_bus(uint16_t *addr, bool *read, bool *sync)
{
    *addr = dut->cpu_addr;
    *read = !dut->cpu_we;
    *sync = dut->rootp->cpu__DOT__cpu_sync;
}

static void rtl_tick(uint8_t *data)
{
    if (dut->cpu_we)
        *data = dut->cpu_data;
    else
        dut->data_i = *data;
    clock_cycle();
}

static void rtl_end(dut_regs_t *regs)
{
    auto *r = dut->rootp;
    regs->pc = r->cpu__DOT__pc;
    regs->s = r->cpu__DOT__s;
    regs->a = r->cpu__DOT__a;
    regs->x = r->cpu__DOT__x;
    regs->y = r->cpu__DOT__y;
    regs->p = r->cpu__DOT__p;
}

static void rtl_pins(bool irq, bool nmi, bool rdy, bool res)
{
    dut->irq_i = irq;
    dut->nmi_i = nmi;
    dut->rdy_i = rdy;
    dut->res_i = res;
}

const dut_t rtl_dut = {
    .name = "w65c02",
    .reset = rtl_reset,
    .begin = rtl_begin,
    .bus = rtl_bus,
    .tick = rtl_tick,
    .end = rtl_end,
    .pins = rtl_pins,
};

void rtl_dut_init(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vcpu;
    dut->en = 1;
    dut->irq_i = 0;
    dut->nmi_i = 0;
    dut->rdy_i = 0;
    dut->res_i = 0;
    rtl_reset();
}

void rtl_dut_free(void)
{
    dut->final();
    delete dut;
    dut = nullptr;
}
