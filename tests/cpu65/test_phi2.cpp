/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The claim the accumulator is there to make: every whole kilohertz the
 * machine allows comes out exact. Count the enables across one full
 * second of system clocks and the count must equal the frequency asked
 * for — not near it, equal to it — for all 7,901 of them.
 *
 * Also the two properties the 6502 depends on and the arithmetic does
 * not state: the enable is never two clocks running, or a cycle would be
 * taken twice; and the gap never falls below what the SDC promises the
 * fitter, which is the whole reason a multicycle path is allowed there.
 */

#include "Vphi2_div.h"

#include "utest.h"

static Vphi2_div *dut;

static const int SYS_KHZ = 50400;

/* The floor the machine's timing constraints are written against. */
static const int SDC_MULTICYCLE = 4;

struct result
{
    long pulses;
    int shortest_gap;
    bool ever_consecutive;
};

static result run_khz(int khz, int sys_clocks)
{
    /* The accumulator has no reset — it is not the 6502 or the 6522, and
     * a rate change is meant to take effect where the phase already is.
     * Each rate therefore starts from power-on, which is the only state
     * the hardware ever establishes for it. */
    delete dut;
    dut = new Vphi2_div;
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
        if (dut->phi2_div_en)
        {
            if (prev)
                r.ever_consecutive = true;
            /* The first pulse measures from reset, not from a pulse. */
            if (r.pulses && gap < r.shortest_gap)
                r.shortest_gap = gap;
            r.pulses++;
            gap = 0;
        }
        prev = dut->phi2_div_en;
    }
    return r;
}

UTEST(phi2, every_khz_is_exact_100_to_8000)
{
    for (int khz = 100; khz <= 8000; khz++)
    {
        result r = run_khz(khz, SYS_KHZ);
        /* One second of system clocks must carry khz thousand pulses.
         * The accumulator starts empty, so the final partial period is
         * still in it and the last pulse has not happened yet. */
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
        /* 50400/8000 is 6.3, so the fastest PHI2 still leaves six. */
        ASSERT_GE(r.shortest_gap, SYS_KHZ / 8000);
    }
}

UTEST(phi2, stopped_when_asked_for_nothing)
{
    /* A zero never reaches the divider from the firmware, which clamps,
     * but the register resets before any firmware runs and a machine
     * that free-ran on an empty register would be worse than a stopped
     * one. */
    result r = run_khz(0, 10000);
    ASSERT_EQ(0L, r.pulses);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    dut = new Vphi2_div;
    int rc = utest_main(argc, argv);
    delete dut;
    return rc;
}
