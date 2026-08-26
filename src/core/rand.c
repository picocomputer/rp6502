/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/rand.h"

/* noinline on purpose. Left to itself, link-time optimization folds this into
 * host_rand_64, then folds that into the syscall dispatcher, and the four
 * finalizer constants land wherever a caller is -- 134 bytes of a soft CPU's
 * 96 KB to save one call. MSVC has no such attribute and no such firmware;
 * compat.h defines it away there. */
__attribute__((noinline)) uint64_t rand_step(uint64_t *state)
{
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t x = *state ^ (*state >> 33);
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}
