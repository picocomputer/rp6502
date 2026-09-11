/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/random.h"
#include "host/host.h"

#include <stdbool.h>
#include <string.h>

/* MSVC has no noinline attribute; osal/windows/msvc/compat.h defines
 * __attribute__ away there. */
__attribute__((noinline)) uint32_t sys_random_step(uint32_t *state)
{
    /* Numerical Recipes' linear congruential generator feeding the lowbias32
     * tuning of Murmur3's 32-bit finalizer. The multiplier is one more than a
     * multiple of four and the increment is odd, so the generator has full
     * period and a state of zero is an ordinary state rather than a fixed
     * point that would need a guard. */
    *state = *state * 1664525u + 1013904223u;
    uint32_t x = *state;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void sys_random_fill(void *dst, size_t len, uint32_t *state)
{
    uint8_t *d = dst;
    for (size_t i = 0; i < len; i += sizeof(uint32_t))
    {
        uint32_t x = sys_random_step(state);
        memcpy(d + i, &x, sizeof x);
    }
}

static uint32_t sys_random_state;
static bool sys_random_seeded;

void sys_random_seed(void)
{
    if (!sys_random_seeded)
    {
        sys_random_state = host_seed();
        sys_random_seeded = true;
    }
}

uint32_t sys_random(void)
{
    sys_random_seed();
    return sys_random_step(&sys_random_state);
}

/* The state is seeded before it is written so that a blob never carries a
 * stream that has not started. A machine loading one would otherwise come up
 * with a state of zero and marked as seeded, while the machine that saved it
 * still draws host_seed() at its first lrand. Seeded rather than drawn from,
 * because a draw would advance the stream and make two saves of one unchanged
 * machine differ. */
void random_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sys_random_seed();
    sst_put_u32(c, sys_random_state);
}

bool random_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint32_t state = sst_get_u32(c);
    if (!sst_ok(c))
        return false;
    sys_random_state = state;
    sys_random_seeded = true;
    return true;
}
