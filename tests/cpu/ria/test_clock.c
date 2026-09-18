/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/wdc/bus.h"
#include "core/wdc/phi2.h"
#include "host/host.h"
#include "emu_boot.h"

static void run_frames(int n)
{
    emu_frames((int)n);
}

UTEST(clock, run_time_is_exact_and_reproducible)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    uint64_t t0 = host_clock_us();
    run_frames(60);
    ASSERT_EQ(host_clock_us() - t0, 1000000ull);

    /* A program restart does not reset the clock, and six frames are 3150
     * scanlines of 2000/63 us each, so they add exactly 100 ms. */
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    run_frames(6);
    ASSERT_EQ(host_clock_us() - t0, 1100000ull);
}

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

    phi2_set_khz_run(50);
    ASSERT_EQ(phi2_get_khz_run(), 100);
    phi2_set_khz_run(20000);
    ASSERT_EQ(phi2_get_khz_run(), 8000);

    for (uint16_t khz = PHI2_MIN_KHZ; khz <= PHI2_MAX_KHZ; khz++)
    {
        phi2_set_khz_run(khz);
        ASSERT_EQ(phi2_get_khz_run(), khz);
    }
}

/* Three frames are 1575 scanlines and a scanline is worth 2*khz/63 cycles,
 * so three frames are exactly 50*khz cycles at every rate. */
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
