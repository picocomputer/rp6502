/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vphi2.h"

#include "utest.h"

#include <vector>

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

static void clock(Vphi2 *d)
{
    d->clk = 0;
    d->eval();
    d->clk = 1;
    d->eval();
}

/* The clocks of the first n enables at khz, held for len clocks from
 * clock at. */
static std::vector<long> steps(int khz, long n, long at, long len)
{
    Vphi2 d;
    d.phi2_khz = khz;
    std::vector<long> v;
    for (long i = 0; (long)v.size() < n; i++)
    {
        d.hold = i >= at && i < at + len;
        clock(&d);
        if (d.phi2_en)
            v.push_back(i);
    }
    return v;
}

UTEST(phi2, a_hold_moves_every_later_step_by_its_length)
{
    static const int probes[] = {1000, 7999, 8000};
    for (int khz : probes)
        for (long at = 100; at < 112; at++)
        {
            std::vector<long> ref = steps(khz, 40, 0, 0);
            std::vector<long> got = steps(khz, 40, at, 37);
            for (size_t i = 0; i < ref.size(); i++)
                ASSERT_EQ(ref[i] < at ? ref[i] : ref[i] + 37, got[i]);
        }
}

/* The count read under a hold and jammed into another counter, held too
 * and out of step with it, makes that one step where the first does. */
UTEST(phi2, a_jammed_count_steps_where_it_was_read)
{
    for (long at = 200; at < 212; at++)
    {
        Vphi2 a, b;
        a.phi2_khz = 7999;
        b.phi2_khz = 1234;
        for (long i = 0; i < at; i++)
        {
            clock(&a);
            clock(&b);
        }
        b.phi2_khz = 7999;
        a.hold = b.hold = 1;
        clock(&a);
        clock(&b);
        b.st_jam = 1;
        b.st_jam_acc = a.phi2_acc;
        b.st_jam_pulse = a.phi2_pulse;
        clock(&b);
        b.st_jam = 0;
        a.hold = b.hold = 0;
        for (int i = 0; i < 2000; i++)
        {
            clock(&a);
            clock(&b);
            ASSERT_EQ(a.phi2_en, b.phi2_en);
        }
    }
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    dut = new Vphi2;
    int rc = utest_main(argc, argv);
    delete dut;
    return rc;
}
