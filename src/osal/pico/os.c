/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What this OS answers of osal/os.h. Only the RIA compiles it: the VGA
 * firmware has none of core/api, so nothing in it reaches for a random
 * number and pico_rand stays out of that image.
 */

#include "osal/os.h"

#include <pico/rand.h>

/* The RP2350's hardware RNG, spent once for the seed rather than on every
 * draw; the shared generator does the rest. */
uint32_t os_random_seed(void)
{
    uint64_t s = get_rand_64();
    return (uint32_t)(s ^ (s >> 32));
}
