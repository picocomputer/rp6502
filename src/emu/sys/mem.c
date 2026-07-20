/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "emu/chips/w65c02.h"
#include "emu/dbg/dbg.h"
#include "emu/sys/mem.h"
#include <stdalign.h>

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

uint64_t mem_tick(uint64_t pins)
{
    uint16_t addr = M6502_GET_ADDR(pins);
    if (pins & M6502_RW)
    {
        M6502_SET_DATA(pins, ram[addr]);
        if (__builtin_expect(dbg_watch_armed, 0))
            dbg_watch_access(addr, ram[addr], false);
    }
    else
    {
        ram[addr] = M6502_GET_DATA(pins);
        if (__builtin_expect(dbg_watch_armed, 0))
            dbg_watch_access(addr, ram[addr], true);
    }
    return pins;
}
