/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/xram.h"
#include "core/sys/random.h"
#include <string.h>

volatile uint8_t xram[0x10000];

static bool xram_fill_random = true;
static uint8_t xram_fill_value;
static uint32_t xram_fill_seed;

/* The savestate and the fill cast volatile away, because memcpy and memset
 * take plain bytes. Nothing else writes xram while they run. */
void xram_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put(c, (const void *)xram, sizeof xram);
}

bool xram_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_get(c, (void *)xram, sizeof xram);
    return sst_ok(c);
}

void xram_set_fill(bool random, uint8_t value, uint32_t seed)
{
    xram_fill_random = random;
    xram_fill_value = value;
    xram_fill_seed = seed;
}

/* The salt is twice the golden-ratio constant sram_init uses, so the two
 * fills of one run's seed do not produce the same 64 KB. */
void xram_init(void)
{
    if (!xram_fill_random)
    {
        memset((void *)xram, xram_fill_value, sizeof xram);
        return;
    }
    uint32_t state = xram_fill_seed ^ 0x3C6EF372u;
    sys_random_fill((void *)xram, sizeof xram, &state);
}
