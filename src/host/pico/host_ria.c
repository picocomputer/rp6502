/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What only the RIA answers of core/host.h. The VGA firmware is a host that
 * does not implement this part, the way a Pico does not implement the fs_*
 * seam -- it compiles none of core/api, so nothing in it reaches for a random
 * number, and linking pico_rand there to satisfy a symbol it never calls would
 * put the RNG in the image that has no use for it.
 *
 * The RP2350's hardware RNG, where the emulator and a Pocket run a generator
 * they can reproduce.
 */

#include "host.h"

#include <pico/rand.h>

uint64_t host_rand_64(void)
{
    return get_rand_64();
}
