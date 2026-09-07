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

// 4KB segments because a single 64KB array crashes my debugger
alignas(XRAM_ALIGN) static uint8_t HOST_UNINITIALIZED_RAM(xram_blocks)[16][0x1000];
volatile uint8_t *const xram = (uint8_t *)xram_blocks;

static bool xram_fill_random = true;
static uint8_t xram_fill_value;
static uint32_t xram_fill_seed;

/* Through xram_blocks, not through xram. The array is plain and only the
 * pointer to it is volatile-qualified, so this is a copy rather than the byte
 * loop a volatile-defined object would need. */
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

/* Its own stream, salted apart from sram's: both fill from the run's seed and
 * must not come up as the same 64 KB twice. */
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
