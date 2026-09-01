/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What only the RIA answers. The VGA firmware compiles none of core/api, so
 * nothing in it reaches for a random number and pico_rand stays out of that
 * image.
 *
 * Both halves of the seed, because this board has an answer of its own: the
 * RP2350's hardware RNG is spent once here rather than on every draw, and the
 * shared generator does the rest.
 */

#include "host/host.h"
#include "osal/os.h"

#include <pico/rand.h>

uint32_t os_random_seed(void)
{
    uint64_t s = get_rand_64();
    return (uint32_t)(s ^ (s >> 32));
}

uint32_t host_random_seed(void)
{
    /* Nothing overrides a board: no command line, no fixture. */
    return os_random_seed();
}
