/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's microsecond clock, which has now been wrong twice in
 * two different ways and both times reached hardware. Left at rv_soc's
 * default 1/1 it ran fifty times fast; divided from clk_sys after the
 * counter moved to clk_rv it ran twice slow. Neither showed up in any
 * test, because nothing else in this suite waits on a real second — the
 * only witness was a cursor blinking at the wrong speed on a Pocket.
 *
 * So assert the rate directly. mtime_us is what host_clock_us returns, and
 * one microsecond is one microsecond of clk_sys wall time no matter
 * which clock the accumulator happens to be counting.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "tb_machine.h"
#include "utest.h"

#include <cstdint>

static Vrp6502 *dut;
static bool rv_phase;

/* One call is one clk_sys period. clk_rv is half of it, rising with it. */

static uint64_t mtime()
{
    return dut->rootp->rp6502__DOT__rv__DOT__mtime_us;
}

UTEST(mtime, counts_real_microseconds)
{
    dut->rst_n = 0;
    tb_clock(dut);
    tb_clock(dut);
    dut->rst_n = 1;
    ASSERT_EQ(mtime(), (uint64_t)0);

    /* Ten milliseconds of clk_sys at 50.4 MHz. The accumulator is exact
     * over any whole hundred microseconds, so this lands on the nose. */
    for (int i = 0; i < 504000; i++)
        tb_clock(dut);
    ASSERT_EQ(mtime(), (uint64_t)10000);
}

UTEST(mtime, is_monotonic_and_never_skips)
{
    /* A wrap that subtracts wrong drifts slowly and would still pass a
     * rate check with a loose bound; stepping by exactly one, always,
     * is the property the accumulator has to hold. */
    dut->rst_n = 0;
    tb_clock(dut);
    tb_clock(dut);
    dut->rst_n = 1;

    uint64_t prev = mtime();
    int steps = 0;
    for (int i = 0; i < 50400; i++)
    {
        tb_clock(dut);
        uint64_t now = mtime();
        if (now == prev)
            continue;
        ASSERT_EQ(now, prev + 1);
        prev = now;
        steps++;
    }
    /* One millisecond of clk_sys is a thousand of them. */
    ASSERT_EQ(steps, 1000);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vrp6502;
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->rst_n = 0;
    dut->eval();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
