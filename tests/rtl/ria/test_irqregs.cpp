/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* SIGINT in regs.sv is bit 6 of the $FFF0 pending byte, so RIA_ATTR_SIGINT and
 * the interrupt are one latch, as on the other machines. The OS sets it with a
 * write of word 27 and takes it with a read of word 27; a $FFF0 read or a
 * $FFF0 write of the bit acknowledges it, and the write of word 17 at the
 * start of a ROM clears it. A word's read value is the one before the edge,
 * which is what the bridge latches, and its side effects land at that edge. */

#include "Vregs.h"
#include "Vregs___024root.h"

#include "utest.h"

static const uint8_t W_IRQ = 17;
static const uint8_t W_SIGINT = 27;
static const uint8_t RS_IRQ = 0x10;
static const uint8_t VSYNC = 0x80;
static const uint8_t SIGINT = 0x40;

static Vregs *dut;

/* One access on the 6502's side, one on the OS's and a VSYNC, or none. */
struct cycle
{
    bool cpu = false;
    bool cpu_write = false;
    uint8_t rs = 0;
    uint8_t data = 0;
    bool os_read = false;
    bool os_write = false;
    uint8_t word = 0;
    uint32_t wdata = 0;
    bool vsync = false;
};

struct seen
{
    uint8_t cpu;
    uint32_t os;
};

static seen step(const cycle &c)
{
    dut->en = c.cpu;
    dut->cs = c.cpu;
    dut->we = c.cpu_write;
    dut->rs = c.rs;
    dut->data_i = c.data;
    dut->vsync_pulse = c.vsync;
    dut->b_re = c.os_read;
    dut->b_we = c.os_write;
    dut->b_word = c.word;
    dut->b_wstrb = c.os_write ? 0xF : 0;
    dut->b_wdata = c.wdata;
    dut->clk = 0;
    dut->clk_mem = 0;
    dut->eval();
    seen s = {dut->regs_data, dut->regs_b_rdata};
    dut->clk = 1;
    dut->clk_mem = 1;
    dut->eval();
    dut->en = 0;
    dut->cs = 0;
    dut->we = 0;
    dut->vsync_pulse = 0;
    dut->b_re = 0;
    dut->b_we = 0;
    dut->clk = 0;
    dut->clk_mem = 0;
    dut->eval();
    return s;
}

static uint8_t cpu_read(uint8_t rs)
{
    cycle c;
    c.cpu = true;
    c.rs = rs;
    return step(c).cpu;
}

static void cpu_write(uint8_t rs, uint8_t v)
{
    cycle c;
    c.cpu = true;
    c.cpu_write = true;
    c.rs = rs;
    c.data = v;
    step(c);
}

static uint32_t os_read(uint8_t word)
{
    cycle c;
    c.os_read = true;
    c.word = word;
    return step(c).os;
}

static void os_write(uint8_t word, uint32_t v)
{
    cycle c;
    c.os_write = true;
    c.word = word;
    c.wdata = v;
    step(c);
}

static void vsync()
{
    cycle c;
    c.vsync = true;
    step(c);
}

static uint8_t pending() { return dut->rootp->regs__DOT__irq_pending; }

static void power_on()
{
    delete dut;
    dut = new Vregs;
    dut->clk = 0;
    dut->clk_mem = 0;
    dut->eval();
    for (int i = 0; i < 4; i++)
        step(cycle());
    os_write(W_IRQ, 0);
}

UTEST(irqregs, ctrl_c_shows_in_fff0_and_its_read_acknowledges)
{
    power_on();
    os_write(W_SIGINT, 0);
    ASSERT_EQ(SIGINT, cpu_read(RS_IRQ));
    ASSERT_EQ(0, pending());
    ASSERT_EQ(0u, os_read(W_SIGINT) & (SIGINT << 8));
}

UTEST(irqregs, the_attribute_takes_it_once)
{
    power_on();
    os_write(W_SIGINT, 0);
    ASSERT_EQ((uint32_t)SIGINT << 8, os_read(W_SIGINT) & 0xFF00);
    ASSERT_EQ(0u, os_read(W_SIGINT) & 0xFF00);
    ASSERT_EQ(0, cpu_read(RS_IRQ));
}

UTEST(irqregs, it_interrupts_only_when_enabled)
{
    power_on();
    cpu_write(RS_IRQ, VSYNC);
    os_write(W_SIGINT, 0);
    ASSERT_FALSE(dut->regs_irq);
    ASSERT_EQ(SIGINT, pending());
    power_on();
    cpu_write(RS_IRQ, SIGINT);
    os_write(W_SIGINT, 0);
    ASSERT_TRUE(dut->regs_irq);
    cpu_read(RS_IRQ);
    ASSERT_FALSE(dut->regs_irq);
}

UTEST(irqregs, enabling_it_acknowledges_one_from_before)
{
    power_on();
    os_write(W_SIGINT, 0);
    cpu_write(RS_IRQ, SIGINT);
    ASSERT_FALSE(dut->regs_irq);
    ASSERT_EQ(0, pending());
}

UTEST(irqregs, the_start_of_a_rom_clears_it)
{
    power_on();
    os_write(W_SIGINT, 0);
    os_write(W_IRQ, 0);
    ASSERT_EQ(0u, os_read(W_SIGINT) & 0xFF00);
}

UTEST(irqregs, taking_it_leaves_vsync)
{
    power_on();
    vsync();
    os_write(W_SIGINT, 0);
    ASSERT_EQ(VSYNC | SIGINT, pending());
    os_read(W_SIGINT);
    ASSERT_EQ(VSYNC, pending());
}

UTEST(irqregs, vsync_and_ctrl_c_on_one_edge_both_land)
{
    power_on();
    cycle c;
    c.vsync = true;
    c.os_write = true;
    c.word = W_SIGINT;
    step(c);
    ASSERT_EQ(VSYNC | SIGINT, pending());
}

UTEST(irqregs, ctrl_c_lands_after_a_same_edge_fff0_read)
{
    power_on();
    cycle c;
    c.cpu = true;
    c.rs = RS_IRQ;
    c.os_write = true;
    c.word = W_SIGINT;
    step(c);
    ASSERT_EQ(SIGINT, pending());
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    int rc = utest_main(argc, argv);
    delete dut;
    return rc;
}
