/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The master clock and PHI2. Everything (CPU cycle budget, VGA scanlines, the
 * s/ds/cs/ms run timers) derives from one 256 MHz master clock advanced by the
 * 6502 ticks, so run time is a reproducible function of the frame count and is
 * independent of PHI2 — exactly what makes timed tests repeatable.
 */

#include "core/wdc/bus.h"
#include "core/wdc/phi2.h"
#include "host/host.h"
#include "emu_boot.h"

static void run_frames(int n)
{
    emu_frames((int)n);
}

/* The master clock advances at the fixed scanline rate regardless of what the
 * CPU does, so run time is a pure function of the frame count: 60 frames =
 * exactly one second, to the microsecond. The clock is machine uptime — it rides
 * through a program restart — so time is measured as a delta. */
UTEST(clock, run_time_is_exact_and_reproducible)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    uint64_t t0 = host_clock_us();
    run_frames(60);
    ASSERT_EQ(host_clock_us() - t0, 1000000ull); /* 60 frames = exactly 1 s */

    /* A program restart does not reset the clock; each frame adds the same fixed
     * quantum, so 6 more frames add exactly 100 ms. */
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    run_frames(6);
    ASSERT_EQ(host_clock_us() - t0, 1100000ull);
}

/* Time is paced by the 60 Hz VGA, not the CPU: a quarter-speed PHI2 runs a
 * quarter of the instructions but the same wall time elapses. */
UTEST(clock, time_is_phi2_independent)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    phi2_set_khz_run(2000);
    uint64_t t0 = host_clock_us();
    run_frames(60);
    ASSERT_EQ(host_clock_us() - t0, 1000000ull);
}

UTEST(clock, phi2_get_set_clamp)
{
    /* Exact divisors (clkdiv 1/2/4/8/80) report back unchanged. */
    phi2_set_khz_run(8000);
    ASSERT_EQ(phi2_get_khz_run(), 8000);
    phi2_set_khz_run(4000);
    ASSERT_EQ(phi2_get_khz_run(), 4000);
    phi2_set_khz_run(2000);
    ASSERT_EQ(phi2_get_khz_run(), 2000);
    phi2_set_khz_run(1000);
    ASSERT_EQ(phi2_get_khz_run(), 1000);
    phi2_set_khz_run(100);
    ASSERT_EQ(phi2_get_khz_run(), 100);

    /* Out of range clamps to [100, 8000]. */
    phi2_set_khz_run(50);
    ASSERT_EQ(phi2_get_khz_run(), 100);
    phi2_set_khz_run(20000);
    ASSERT_EQ(phi2_get_khz_run(), 8000);

    /* Every whole kilohertz in range is exact here, as in phi2.sv. The
     * board's PIO divider cannot do this and lands nearby instead. */
    for (uint16_t khz = PHI2_MIN_KHZ; khz <= PHI2_MAX_KHZ; khz++)
    {
        phi2_set_khz_run(khz);
        ASSERT_EQ(phi2_get_khz_run(), khz);
    }
}

/* How many cycles a frame is worth, which no other test looks at and which
 * the machine cannot be asked from outside. Three frames is 1575 scanlines
 * and a scanline is 2*khz/63 cycles, so three frames is exactly 50*khz at
 * every rate -- an integer whatever the accumulator is doing underneath.
 *
 * 100 and 733 are the interesting ones: 8000 divides the tick count evenly
 * and would pass even if the fraction were dropped on the floor. */
UTEST(clock, cycles_per_frame_is_exact)
{
    static const uint16_t rates[] = {100, 733, 8000};
    for (unsigned i = 0; i < sizeof rates / sizeof *rates; i++)
    {
        ASSERT_TRUE(emu_restart(TEST_FIXTURE));
        phi2_set_khz_run(rates[i]);
        uint64_t c0 = bus_cycles();
        run_frames(3);
        ASSERT_EQ(bus_cycles() - c0, 50ull * rates[i]);
    }
}

UTEST_MAIN_EMU()
