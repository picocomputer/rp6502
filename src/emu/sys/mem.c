/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "emu/sys/mem.h"
#include <assert.h>
#include <stdalign.h>
#include <string.h>

uint8_t ram[0x10000];

static uint8_t xram_mem[0x10000];
uint8_t *const xram = xram_mem;

alignas(4) volatile uint8_t regs[0x20];

uint8_t xstack[XSTACK_SIZE + 1];
volatile size_t xstack_ptr = XSTACK_SIZE;

volatile uint8_t xram_queue_page;
volatile uint8_t xram_queue_head;
volatile uint8_t xram_queue_tail;
volatile uint8_t xram_queue[256][2];

/* mem_fill writes whole words, so both are a multiple of one. */
static_assert(!(sizeof(ram) % sizeof(uint64_t)));
static_assert(!(sizeof(xram_mem) % sizeof(uint64_t)));

static bool mem_fill_random = true;
static uint8_t mem_fill_value;
static uint64_t mem_fill_seed;

void mem_set_fill(bool random, uint8_t value, uint64_t seed)
{
    mem_fill_random = random;
    mem_fill_value = value;
    mem_fill_seed = seed;
}

/* The fill gets its own stream instead of drawing from get_rand_64, which is
 * what the 6502's rand() syscall reads: 128KB of draws would move the sequence
 * every seeded program sees, and then changing the fill would be changing two
 * things at once. Same LCG and finalizer as app/rand.c, salted off the run's
 * seed by the golden ratio so the two streams start apart. */
static void mem_fill(uint8_t *dst, size_t len, uint64_t *state)
{
    for (size_t i = 0; i < len; i += sizeof(uint64_t))
    {
        *state = *state * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t x = *state ^ (*state >> 33);
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        memcpy(dst + i, &x, sizeof x);
    }
}

void mem_init(void)
{
    if (!mem_fill_random)
    {
        memset(ram, mem_fill_value, sizeof ram);
        memset(xram_mem, mem_fill_value, sizeof xram_mem);
        return;
    }
    uint64_t state = mem_fill_seed ^ 0x9E3779B97F4A7C15ull;
    mem_fill(ram, sizeof ram, &state);
    mem_fill(xram_mem, sizeof xram_mem, &state);
}

/* The SRAM's bus cycle. Every write lands — ram[] shadows the whole space, which is
 * what the debug memory views and the ROM loader read — but only $0000-$FEFF drives
 * the bus on a read (os.rst). Above that the VIA and RIA answer, and the unassigned
 * $FF00-$FFCF reads as open bus: nothing drives it, so data keeps what the CPU left. */
void mem_tick(uint16_t addr, bool read, uint8_t *data)
{
    if (!read)
        ram[addr] = *data;
    else if (addr <= MEM_MMAP_HI)
        *data = ram[addr];
}

/* Standalone CRC-32/ISO-HDLC (zlib): the firmware reuses littlefs's lfs_crc, but
 * the emulator doesn't link littlefs. Same polynomial, so the values match the
 * .rp6502 headers and the firmware. */
uint32_t mem_crc32(uint32_t crc, const void *buf, size_t len)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    crc ^= 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
