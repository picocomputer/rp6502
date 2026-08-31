/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What only the RIA answers of host/os.h. The VGA firmware compiles none of
 * core/api, so nothing in it reaches for a random number and pico_rand stays
 * out of that image.
 *
 * The RP2350's hardware RNG.
 */

#include "host.h"

#include <pico/rand.h>

uint64_t host_rand_64(void)
{
    return get_rand_64();
}
