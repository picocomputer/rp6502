/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The byte the OS offers the 6502 through regs.sv, and the two words the OS
 * reads to look at it and to take it back: RX_OFFER at word 18, RX_PEEK at
 * word 25 and RX_RECLAIM at word 26. A word's read value is the one before
 * the edge, which is what the bridge latches, and its side effects land at
 * that edge. */

#include "Vregs.h"
#include "Vregs___024root.h"

#include "utest.h"

static const uint8_t W_OFFER = 18;
static const uint8_t W_PEEK = 25;
static const uint8_t W_RECLAIM = 26;
static const uint8_t RX_READY = 0x40;

static Vregs *dut;

/* One access on the 6502's side and one on the OS's, or neither. */
struct cycle
{
    bool cpu = false;
    uint8_t rs = 0;
    bool os_read = false;
    bool os_write = false;
    uint8_t word = 0;
    uint32_t wdata = 0;
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
    dut->we = 0;
    dut->rs = c.rs;
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

/* A $FFE0 read that finds nothing sets the request, and the OS answers it
 * with a byte. */
static void offer(uint8_t ch)
{
    cpu_read(0x00);
    os_write(W_OFFER, ch);
}

static uint8_t reg(int i) { return dut->rootp->regs__DOT__regs[i]; }
static bool slot() { return dut->rootp->regs__DOT__os_rx_valid; }
static bool req() { return dut->rootp->regs__DOT__rx_req; }

static void power_on()
{
    delete dut;
    dut = new Vregs;
    dut->clk = 0;
    dut->clk_mem = 0;
    dut->eval();
    for (int i = 0; i < 4; i++)
        step(cycle());
}

UTEST(rxregs, a_read_that_finds_nothing_asks)
{
    power_on();
    ASSERT_EQ(1u, os_read(W_OFFER) & 3);
    cpu_read(0x00);
    ASSERT_EQ(3u, os_read(W_OFFER) & 3);
}

UTEST(rxregs, a_peek_takes_nothing)
{
    power_on();
    offer('a');
    ASSERT_EQ(0x161u, os_read(W_PEEK));
    ASSERT_EQ(0x161u, os_read(W_PEEK));
    ASSERT_TRUE(slot());
    ASSERT_FALSE(req());
}

UTEST(rxregs, a_reclaim_empties_the_offer_slot)
{
    power_on();
    offer('a');
    ASSERT_EQ(0x161u, os_read(W_RECLAIM));
    ASSERT_FALSE(slot());
    ASSERT_EQ(0u, os_read(W_PEEK));
    ASSERT_EQ(0, cpu_read(0x00) & RX_READY);
    ASSERT_TRUE(req());
}

UTEST(rxregs, a_reclaim_empties_the_latch)
{
    power_on();
    offer('a');
    ASSERT_NE(0, cpu_read(0x00) & RX_READY);
    ASSERT_FALSE(slot());
    ASSERT_EQ(0x161u, os_read(W_PEEK));
    ASSERT_EQ(0x161u, os_read(W_RECLAIM));
    ASSERT_EQ(0, reg(0) & RX_READY);
    ASSERT_EQ(0, reg(2));
    ASSERT_EQ(0, cpu_read(0x02));
    ASSERT_TRUE(req());
}

UTEST(rxregs, the_6502_wins_the_latch_on_the_same_edge)
{
    power_on();
    offer('a');
    cpu_read(0x00);
    cycle c;
    c.cpu = true;
    c.rs = 0x02;
    c.os_read = true;
    c.word = W_RECLAIM;
    seen s = step(c);
    ASSERT_EQ('a', s.cpu);
    ASSERT_EQ(0u, s.os & 0x100);
    ASSERT_EQ(0u, os_read(W_PEEK));
}

UTEST(rxregs, the_os_wins_the_slot_from_a_same_edge_ffe2_read)
{
    power_on();
    offer('a');
    cycle c;
    c.cpu = true;
    c.rs = 0x02;
    c.os_read = true;
    c.word = W_RECLAIM;
    seen s = step(c);
    ASSERT_EQ(0, s.cpu);
    ASSERT_EQ(0x161u, s.os);
    ASSERT_FALSE(slot());
    ASSERT_EQ(0, reg(0) & RX_READY);
    ASSERT_EQ(0, reg(2));
}

UTEST(rxregs, the_os_wins_the_slot_from_a_same_edge_ffe0_read)
{
    power_on();
    offer('a');
    cycle c;
    c.cpu = true;
    c.rs = 0x00;
    c.os_read = true;
    c.word = W_RECLAIM;
    seen s = step(c);
    ASSERT_NE(0, s.cpu & RX_READY);
    ASSERT_EQ(0x161u, s.os);
    ASSERT_FALSE(slot());
    ASSERT_EQ(0, reg(0) & RX_READY);
    ASSERT_EQ(0u, os_read(W_PEEK));
    ASSERT_EQ(0, cpu_read(0x02));
    ASSERT_TRUE(req());
}

UTEST(rxregs, a_reclaim_of_nothing_changes_nothing)
{
    power_on();
    cpu_read(0x00);
    uint8_t r0 = reg(0), r2 = reg(2);
    ASSERT_EQ(0u, os_read(W_RECLAIM));
    ASSERT_TRUE(req());
    ASSERT_EQ(r0, reg(0));
    ASSERT_EQ(r2, reg(2));
}

UTEST(rxregs, the_offer_word_keeps_its_layout)
{
    power_on();
    cpu_read(0x00);
    ASSERT_EQ(3u, os_read(W_OFFER) & 0xFF03);
    os_write(W_OFFER, 0x200);
    ASSERT_FALSE(req());
    ASSERT_FALSE(slot());
    offer('a');
    ASSERT_EQ(0x6100u, os_read(W_OFFER) & 0xFF03);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    int rc = utest_main(argc, argv);
    delete dut;
    return rc;
}
