/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host stand-in for the pico-sdk's hardware RNG (get_rand_64), the entropy
 * source the vendored firmware atr.c uses for lrand. On real hardware lrand is
 * free-running entropy that a reset/exec never restarts, so the emulator
 * defaults to real host entropy too (seeded lazily on first use) — a game that
 * wants randomness gets it. For reproducible runs (tests, debugging) a fixed
 * seed can be forced with emu_set_random_seed (the --seed CLI option); the
 * generator is unchanged, so a given seed always replays the same stream.
 */

#include "emu/host/rand.h"
#include "pico/rand.h"
#include <stdbool.h>
#include <time.h>
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <sys/random.h>
#endif

/* An LCG step (the PCG/musl multiplier) feeding a Murmur3 fmix64 finalizer: cheap,
 * full-period, and well-distributed across all 64 output bits. */
static uint64_t rand_state;
static bool rand_seeded;

/* Gather a 64-bit seed from the host. Prefers the kernel RNG; falls back (and
 * on the web, where there is no getrandom) to mixing the monotonic and realtime
 * clocks with a stack address — enough to differ run-to-run. */
static uint64_t rand_entropy(void)
{
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    {
        uint64_t s;
        if (getrandom(&s, sizeof s, 0) == (ssize_t)sizeof s && s)
            return s;
    }
#endif
    struct timespec mono = {0}, real = {0};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    uint64_t s = (uint64_t)mono.tv_nsec * 6364136223846793005ull +
                 (uint64_t)real.tv_sec * 1442695040888963407ull +
                 (uint64_t)real.tv_nsec + (uint64_t)(uintptr_t)&mono;
    return s ? s : 1;
}

void emu_set_random_seed(uint64_t seed)
{
    rand_state = seed ? seed : 1; /* 0 would freeze the LCG warm-up */
    rand_seeded = true;
}

uint64_t get_rand_64(void)
{
    if (!rand_seeded)
        emu_set_random_seed(rand_entropy());
    rand_state = rand_state * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t x = rand_state ^ (rand_state >> 33);
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}
