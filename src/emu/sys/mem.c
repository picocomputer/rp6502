/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "emu/sys/mem.h"
#include <stdalign.h>
#include <stdio.h>

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

/* The fabric requires a 32-bit aligned base and has no shifter to forgive one. This
 * host still can, so a halfword base keeps working and names itself instead — a
 * program that only ever runs here would otherwise meet the rule for the first time
 * on a Pocket. An odd base was never fetchable and is still refused.
 *
 * Define RP6502_XRAM_ALIGN_ERRNO to refuse both and answer through the xreg. */
bool mem_xram_align(uint16_t addr)
{
    if (!(addr & 3))
        return true;
#ifndef RP6502_XRAM_ALIGN_ERRNO
    if (!(addr & 1))
    {
        printf("?Deprecated XRAM alignment 0x%04X\n", addr);
        return true;
    }
#endif
    return false;
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
