/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "tb_machine.h"
#include "utest.h"

#include <cstdint>

static Vwiring *dut;

static uint64_t mtime()
{
    return dut->rootp->wiring__DOT__soc__DOT__mtime_us;
}

UTEST(mtime, counts_real_microseconds)
{
    dut->rst_n = 0;
    tb_clock(dut);
    tb_clock(dut);
    dut->rst_n = 1;
    ASSERT_EQ(mtime(), (uint64_t)0);

    /* One tb_clock call is one clk_sys period, so 504000 calls are ten
     * milliseconds at the default 50.4 MHz. Ten microseconds is 504
     * clocks, which add 5040 to the accumulator, exactly ten wraps at
     * 504, so the count is exactly 10000. */
    for (int i = 0; i < 504000; i++)
        tb_clock(dut);
    ASSERT_EQ(mtime(), (uint64_t)10000);
}

UTEST(mtime, is_monotonic_and_never_skips)
{
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
    ASSERT_EQ(steps, 1000);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vwiring;
    dut->clk_sys = 0;
    dut->rst_n = 0;
    dut->eval();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
