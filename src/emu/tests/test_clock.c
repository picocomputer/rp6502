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

#include "emu/mon/rom.h"
#include "emu/sys/sys.h"
#include "utest.h"

static void run_frames(int n)
{
    for (int i = 0; i < n; i++)
        emu_run_frame();
}

/* hello exits early; the master clock then advances at the fixed scanline rate.
 * 60 frames = exactly one second of run time, to the microsecond. */
UTEST(clock, run_time_is_exact_and_reproducible)
{
    ASSERT_TRUE(emu_rom_load(HELLO_ROM));
    emu_init();
    run_frames(60);
    ASSERT_EQ(emu_now_us(), 1000000ull);

    /* A second identical run yields the identical time. */
    ASSERT_TRUE(emu_rom_load(HELLO_ROM));
    emu_init();
    run_frames(6);
    ASSERT_EQ(emu_now_us(), 100000ull); /* 100 ms */
}

/* Time is paced by the 60 Hz VGA, not the CPU: a quarter-speed PHI2 runs a
 * quarter of the instructions but the same wall time elapses. */
UTEST(clock, time_is_phi2_independent)
{
    ASSERT_TRUE(emu_rom_load(HELLO_ROM));
    emu_init();
    emu_set_phi2_khz(2000);
    run_frames(60);
    ASSERT_EQ(emu_now_us(), 1000000ull);
}

UTEST(clock, phi2_get_set_clamp)
{
    /* Exact divisors (clkdiv 1/2/4/8/80) report back unchanged. */
    emu_set_phi2_khz(8000);
    ASSERT_EQ(emu_get_phi2_khz(), 8000);
    emu_set_phi2_khz(4000);
    ASSERT_EQ(emu_get_phi2_khz(), 4000);
    emu_set_phi2_khz(2000);
    ASSERT_EQ(emu_get_phi2_khz(), 2000);
    emu_set_phi2_khz(1000);
    ASSERT_EQ(emu_get_phi2_khz(), 1000);
    emu_set_phi2_khz(100);
    ASSERT_EQ(emu_get_phi2_khz(), 100);

    /* Out of range clamps to [100, 8000]. */
    emu_set_phi2_khz(50);
    ASSERT_EQ(emu_get_phi2_khz(), 100);
    emu_set_phi2_khz(20000);
    ASSERT_EQ(emu_get_phi2_khz(), 8000);

    /* An unrepresentable rate quantizes to a nearby achievable one. */
    emu_set_phi2_khz(3000);
    ASSERT_TRUE(emu_get_phi2_khz() >= 2950 && emu_get_phi2_khz() <= 3050);
}

UTEST_MAIN()
