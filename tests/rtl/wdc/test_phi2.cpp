/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vphi2.h"

#include "utest.h"

static Vphi2 *dut;

static const int SYS_KHZ = 50400;

/* The four-clock multicycles in src/host/pocket/quartus/wiring.sdc, on
 * paths from and to the 6502 and the VIA, require consecutive enables to
 * be at least four system clocks apart. */
static const int SDC_MULTICYCLE = 4;

struct result
{
    long pulses;
    int shortest_gap;
    bool ever_consecutive;
};

static result run_khz(int khz, int sys_clocks)
{
    delete dut;
    dut = new Vphi2;
    dut->phi2_khz = khz;
    dut->clk = 0;
    dut->eval();

    result r = {0, 1 << 30, false};
    int gap = 0;
    bool prev = false;
    for (int i = 0; i < sys_clocks; i++)
    {
        dut->clk = 0;
        dut->eval();
        dut->clk = 1;
        dut->eval();
        gap++;
        if (dut->phi2_en)
        {
            if (prev)
                r.ever_consecutive = true;
            if (r.pulses && gap < r.shortest_gap)
                r.shortest_gap = gap;
            r.pulses++;
            gap = 0;
        }
        prev = dut->phi2_en;
    }
    return r;
}

UTEST(phi2, every_khz_is_exact_100_to_8000)
{
    for (int khz = 100; khz <= 8000; khz++)
    {
        result r = run_khz(khz, SYS_KHZ);
        ASSERT_EQ((long)khz, r.pulses);
    }
}

UTEST(phi2, never_two_clocks_running_and_never_tighter_than_the_sdc)
{
    static const int probes[] = {100, 101, 999, 1000, 4096, 7999, 8000};
    for (int khz : probes)
    {
        result r = run_khz(khz, SYS_KHZ / 8);
        ASSERT_FALSE(r.ever_consecutive);
        ASSERT_GE(r.shortest_gap, SDC_MULTICYCLE);
        ASSERT_GE(r.shortest_gap, SYS_KHZ / 8000);
    }
}

UTEST(phi2, stopped_when_asked_for_nothing)
{
    result r = run_khz(0, 10000);
    ASSERT_EQ(0L, r.pulses);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    dut = new Vphi2;
    int rc = utest_main(argc, argv);
    delete dut;
    return rc;
}
