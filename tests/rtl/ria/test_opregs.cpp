/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Two ops finish in regs.sv itself, inside the 6502's write to $FFEF, as the
 * other machines finish them inside the write. DROP ($00) empties the xstack
 * and releases with A = X = 0, so the 6502 can push on the very next cycle.
 * EXIT ($FF) raises regs_hold, which stalls the 6502, until the OS releases
 * $FFF2 with the 6502 already in reset. Word 200 is the xstack pointer and
 * word 192 the guard word at the top of the xstack. */

#include "Vregs.h"
#include "Vregs___024root.h"

#include "utest.h"

static const uint8_t W_CALL = 4; /* $FFF0-$FFF3 */
static const uint8_t W_GUARD = 192;
static const uint8_t W_XSP = 200;
static const uint8_t RS_XSTACK = 0x0C;
static const uint8_t RS_OP = 0x0F;

static Vregs *dut;

static void tick(void)
{
    dut->clk = 0;
    dut->clk_mem = 0;
    dut->eval();
    dut->clk = 1;
    dut->clk_mem = 1;
    dut->eval();
    dut->en = 0;
    dut->cs = 0;
    dut->we = 0;
    dut->b_re = 0;
    dut->b_we = 0;
    dut->clk = 0;
    dut->clk_mem = 0;
    dut->eval();
}

static uint8_t cpu_read(uint8_t rs)
{
    dut->en = 1;
    dut->cs = 1;
    dut->rs = rs;
    dut->clk = 0;
    dut->eval();
    uint8_t v = dut->regs_data;
    tick();
    return v;
}

static void cpu_write(uint8_t rs, uint8_t v)
{
    dut->en = 1;
    dut->cs = 1;
    dut->we = 1;
    dut->rs = rs;
    dut->data_i = v;
    tick();
}

static uint32_t os_read(uint8_t word)
{
    dut->b_re = 1;
    dut->b_word = word;
    dut->clk = 0;
    dut->eval();
    uint32_t v = dut->regs_b_rdata;
    tick();
    return v;
}

static void os_write(uint8_t word, uint32_t v)
{
    dut->b_we = 1;
    dut->b_word = word;
    dut->b_wstrb = 0xF;
    dut->b_wdata = v;
    tick();
}

static void power_on(void)
{
    delete dut;
    dut = new Vregs;
    for (int i = 0; i < 4; i++)
        tick();
    os_write(W_GUARD, 0x5A);
    os_write(W_XSP, 512);
}

UTEST(opregs, drop_empties_the_xstack_in_its_own_cycle)
{
    power_on();
    cpu_write(RS_XSTACK, 1);
    cpu_write(RS_XSTACK, 2);
    cpu_write(RS_XSTACK, 3);
    ASSERT_EQ(509u, os_read(W_XSP) & 0x3FF);
    cpu_write(RS_OP, 0x00);
    ASSERT_FALSE(dut->regs_api_pending);
    ASSERT_FALSE(dut->regs_hold);
    ASSERT_EQ(512u, os_read(W_XSP) & 0x3FF);
    ASSERT_EQ(0x00, cpu_read(0x12));
    ASSERT_EQ(0x00, cpu_read(0x14));
    ASSERT_EQ(0x00, cpu_read(0x16));
    ASSERT_EQ(0x5A, cpu_read(RS_XSTACK));
}

UTEST(opregs, a_push_right_after_drop_lands)
{
    power_on();
    cpu_write(RS_XSTACK, 1);
    cpu_write(RS_OP, 0x00);
    cpu_write(RS_XSTACK, 0x42);
    ASSERT_EQ(511u, os_read(W_XSP) & 0x3FF);
    ASSERT_EQ(0x42, cpu_read(RS_XSTACK));
}

UTEST(opregs, exit_holds_the_6502_until_released)
{
    power_on();
    cpu_write(RS_OP, 0xFF);
    ASSERT_TRUE(dut->regs_hold);
    ASSERT_TRUE(dut->regs_api_pending);
    uint32_t call = os_read(W_CALL);
    os_write(W_CALL, call & 0xFF00FFFFu);
    ASSERT_FALSE(dut->regs_hold);
}

UTEST(opregs, another_op_does_not_hold)
{
    power_on();
    cpu_write(RS_OP, 0x01);
    ASSERT_FALSE(dut->regs_hold);
    ASSERT_TRUE(dut->regs_api_pending);
}

UTEST_MAIN()
