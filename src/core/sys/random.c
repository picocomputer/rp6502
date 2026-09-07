/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/random.h"
#include "host/host.h"

#include <stdbool.h>
#include <string.h>

/* noinline on purpose. Left to itself, link-time optimization folds this into
 * sys_random, then folds that into the syscall dispatcher, and the finalizer's
 * constants land wherever a caller is -- bytes of a soft CPU's 96 KB to save
 * one call. MSVC has no such attribute and no such firmware; compat.h defines
 * it away there. */
__attribute__((noinline)) uint32_t sys_random_step(uint32_t *state)
{
    /* Numerical Recipes' LCG into Murmur3's 32-bit finalizer as retuned by
     * lowbias32: full period, and every output bit well mixed. The increment
     * is odd, so a state of zero is an ordinary state and needs no guard --
     * which matters now that a fixture may pin a seed of zero. */
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

/* Seeded first, so a blob never carries a stream that has not started: the
 * machine that loads one would otherwise draw its own entropy at the first
 * rand() and two peers would part company there. It is the only thing a save
 * changes about the machine, and it changes it identically every time, so two
 * saves of one unchanged machine are still the same bytes. */
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
