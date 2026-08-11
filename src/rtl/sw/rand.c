/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mmio.h"
#include "rand.h"

#include <pico/rand.h>

static uint64_t rand_state = 1;

void rand_init(void)
{
    uint64_t seed = RTC_VALID ? RTC_EPOCH : 0;
    rand_state = seed ? seed : 1;
}

uint64_t get_rand_64(void)
{
    rand_state = rand_state * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t x = rand_state ^ (rand_state >> 33);
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}
