/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator's generator (emu/app/rand.c): an LCG step feeding a
 * Murmur3 fmix64 finalizer. The state starts at the emulator's seeded
 * origin so a simulation and its oracle draw the same stream; platform
 * entropy arrives with the hardware bring-up.
 */

#include <pico/rand.h>

static uint64_t rand_state = 1;

uint64_t get_rand_64(void)
{
    rand_state = rand_state * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t x = rand_state ^ (rand_state >> 33);
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}
