/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/app/rand.h"
#include "emu/host/host.h"
#include "pico/rand.h"
#include <stdbool.h>
#include <stdint.h>

/* An LCG step (the PCG/musl multiplier) feeding a Murmur3 fmix64 finalizer: cheap,
 * full-period, and well-distributed across all 64 output bits. */
static uint64_t rand_state;
static bool rand_seeded;

void rand_set_seed(uint64_t seed)
{
    rand_state = seed ? seed : 1; /* 0 would freeze the LCG warm-up */
    rand_seeded = true;
}

uint64_t get_rand_64(void)
{
    if (!rand_seeded)
        rand_set_seed(os_entropy_64());
    rand_state = rand_state * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t x = rand_state ^ (rand_state >> 33);
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}
