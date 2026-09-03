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

void sram_set_fill(bool random, uint8_t value, uint32_t seed)
{
    sram_fill_random = random;
    sram_fill_value = value;
    sram_fill_seed = seed;
}

/* The fill gets its own stream instead of drawing from sys_random, which is
 * what the 6502's rand() syscall reads: 64 KB of draws would move the sequence
 * every seeded program sees, and then changing the fill would be changing two
 * things at once. Salted off the run's seed by the golden ratio so the streams
 * start apart; xram's takes the next multiple. */
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

/* The SRAM's bus cycle. Every write lands -- sram[] shadows the whole space,
 * which is what the debug memory views and the ROM loader read -- but only
 * $0000-$FEFF drives the bus on a read (os.rst). Above that the VIA and RIA
 * answer, and the unassigned $FF00-$FFCF reads as open bus: nothing drives it,
 * so data keeps what the CPU left. */
void sram_tick(uint16_t addr, bool read, uint8_t *data)
{
    if (!read)
        sram[addr] = *data;
    else if (addr <= SRAM_MMAP_HI)
        *data = sram[addr];
}
