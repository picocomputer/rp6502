/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/xram.h"
#include "core/sys/random.h"
#include "machine.h"
#include <stdalign.h>
#include <string.h>

/* The 64 KB is declared as sixteen 4 KB blocks because a single 64 KB array
 * crashes the debugger. */
alignas(XRAM_ALIGN) static uint8_t HOST_UNINITIALIZED_RAM(xram_blocks)[16][0x1000];
volatile uint8_t *const xram = (uint8_t *)xram_blocks;

static bool xram_fill_random = true;
static uint8_t xram_fill_value;
static uint32_t xram_fill_seed;

/* The cursor is given xram_blocks and not xram because xram points to
 * volatile bytes, and memcpy cannot take a volatile source. */
void xram_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put(c, xram_blocks, sizeof xram_blocks);
}

bool xram_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_get(c, xram_blocks, sizeof xram_blocks);
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
        memset(xram_blocks, xram_fill_value, sizeof xram_blocks);
        return;
    }
    uint32_t state = xram_fill_seed ^ 0x3C6EF372u;
    sys_random_fill(xram_blocks, sizeof xram_blocks, &state);
}
