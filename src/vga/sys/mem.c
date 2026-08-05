/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga/sys/mem.h"
#include <pico.h>
#include <stdalign.h>
#include <stdio.h>

// 4KB segments because a single 64KB array crashes my debugger
alignas(0x10000) static uint8_t __uninitialized_ram(xram_blocks)[16][0x1000];
volatile uint8_t *const xram = (uint8_t *)xram_blocks;

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
