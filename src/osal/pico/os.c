/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What this OS answers of osal/os.h. Only the RIA compiles it: the VGA
 * firmware has none of core/api, so nothing in it reaches for a random
 * number and pico_rand stays out of that image -- and nothing in it asks
 * the time either, because the one deadline it keeps is a blink, which
 * counts frames.
 */

#include "osal/os.h"

#include <pico/rand.h>
#include <pico/time.h>

/* TIMER0, free-running since reset. The same counter host_clock_us reads:
 * this board's machine time is real time, so one counter answers both, and
 * saying so is better than a second reading of the same register. */
uint64_t os_mono_ns(void)
{
    return time_us_64() * 1000;
}

/* The RP2350's hardware RNG, spent once for the seed rather than on every
 * draw; the shared generator does the rest. */
uint32_t os_random(void)
{
    uint64_t s = get_rand_64();
    return (uint32_t)(s ^ (s >> 32));
}
