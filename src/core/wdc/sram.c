/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/wdc/sram.h"
#include "core/sys/random.h"
#include <string.h>

uint8_t sram[0x10000];

static bool sram_fill_random = true;
static uint8_t sram_fill_value;
static uint32_t sram_fill_seed;

void sram_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put(c, sram, sizeof sram);
}

bool sram_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_get(c, sram, sizeof sram);
    return sst_ok(c);
}

void sram_set_fill(bool random, uint8_t value, uint32_t seed)
{
    sram_fill_random = random;
    sram_fill_value = value;
    sram_fill_seed = seed;
}

/* The fill draws from its own stream rather than from sys_random, which is
 * what the 6502's lrand syscall reads, because 64 KB of draws would move the
 * sequence every seeded program sees. The seed is salted with the golden ratio
 * constant 0x9E3779B9 so that the two streams start apart; xram_init salts
 * with twice that. */
void sram_init(void)
{
    if (!sram_fill_random)
    {
        memset(sram, sram_fill_value, sizeof sram);
        return;
    }
    uint32_t state = sram_fill_seed ^ 0x9E3779B9u;
    sys_random_fill(sram, sizeof sram, &state);
}

/* Every write lands, whatever the address, because sram[] shadows the whole
 * space for the debug memory views and the ROM loader. Only $0000-$FEFF drives
 * the bus on a read (os.rst): above that the VIA and the RIA answer, and the
 * unassigned $FF00-$FFCF reads as open bus, so data keeps whatever the CPU
 * left there. */
void sram_tick(uint16_t addr, bool read, uint8_t *data)
{
    if (!read)
        sram[addr] = *data;
    else if (addr <= SRAM_MMAP_HI)
        *data = sram[addr];
}
