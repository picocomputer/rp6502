/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator's generator (emu/app/rand.c): an LCG step feeding a
 * Murmur3 fmix64 finalizer. Seeded at boot from the wall clock the
 * host wrote — the only entropy this machine has. With no clock the
 * state starts at 1, which is what the emulator's rand_set_seed(0)
 * produces, so a test that wants both machines on one stream can pin
 * the oracle to it.
 */

#include "mmio.h"
#include "rand.h"

#include <pico/rand.h>

static uint64_t rand_state = 1;

void rand_init(void)
{
    uint64_t seed = RTC_VALID ? RTC_EPOCH : 0;
    rand_state = seed ? seed : 1; /* 0 would freeze the LCG warm-up */
}

uint64_t get_rand_64(void)
{
    rand_state = rand_state * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t x = rand_state ^ (rand_state >> 33);
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}
