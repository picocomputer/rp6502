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

#include "emu/sys/cpu.h"
#include "pico/time.h"
#include "emu_boot.h"

static void run_frames(int n)
{
    for (int i = 0; i < n; i++)
        sys_run_frame();
}

/* The master clock advances at the fixed scanline rate regardless of what the
 * CPU does, so run time is a pure function of the frame count: 60 frames =
 * exactly one second, to the microsecond. The clock is machine uptime — it rides
 * through a program restart — so time is measured as a delta. */
UTEST(clock, run_time_is_exact_and_reproducible)
{
    ASSERT_TRUE(emu_restart(ADVENTURE_ROM));
    uint64_t t0 = time_us_64();
    run_frames(60);
    ASSERT_EQ(time_us_64() - t0, 1000000ull); /* 60 frames = exactly 1 s */

    /* A program restart does not reset the clock; each frame adds the same fixed
     * quantum, so 6 more frames add exactly 100 ms. */
    ASSERT_TRUE(emu_restart(ADVENTURE_ROM));
    run_frames(6);
    ASSERT_EQ(time_us_64() - t0, 1100000ull);
}

/* Time is paced by the 60 Hz VGA, not the CPU: a quarter-speed PHI2 runs a
 * quarter of the instructions but the same wall time elapses. */
UTEST(clock, time_is_phi2_independent)
{
    ASSERT_TRUE(emu_restart(ADVENTURE_ROM));
    cpu_set_phi2_khz_run(2000);
    uint64_t t0 = time_us_64();
    run_frames(60);
    ASSERT_EQ(time_us_64() - t0, 1000000ull);
}

UTEST(clock, phi2_get_set_clamp)
{
    /* Exact divisors (clkdiv 1/2/4/8/80) report back unchanged. */
    cpu_set_phi2_khz_run(8000);
    ASSERT_EQ(cpu_get_phi2_khz_run(), 8000);
    cpu_set_phi2_khz_run(4000);
    ASSERT_EQ(cpu_get_phi2_khz_run(), 4000);
    cpu_set_phi2_khz_run(2000);
    ASSERT_EQ(cpu_get_phi2_khz_run(), 2000);
    cpu_set_phi2_khz_run(1000);
    ASSERT_EQ(cpu_get_phi2_khz_run(), 1000);
    cpu_set_phi2_khz_run(100);
    ASSERT_EQ(cpu_get_phi2_khz_run(), 100);

    /* Out of range clamps to [100, 8000]. */
    cpu_set_phi2_khz_run(50);
    ASSERT_EQ(cpu_get_phi2_khz_run(), 100);
    cpu_set_phi2_khz_run(20000);
    ASSERT_EQ(cpu_get_phi2_khz_run(), 8000);

    /* An unrepresentable rate quantizes to a nearby achievable one. */
    cpu_set_phi2_khz_run(3000);
    ASSERT_TRUE(cpu_get_phi2_khz_run() >= 2950 && cpu_get_phi2_khz_run() <= 3050);
}

UTEST_MAIN_EMU()
