/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pocket port of ria/sys/mem.c, so far as the API needs it. The regs
 * symbol, the xstack and its pointer are all hardware behind the OS window,
 * placed by the linker script.
 *
 * memcpy and memset live here because link-time code generation emits
 * calls to them after it has decided what to keep, so picolibc's never
 * get the chance to take over.
 */

#include "ria/main.h"
#include "ria/sys/mem.h"

#include <stdio.h>

/* The XRAM is hardware behind its window, like the register cells. */
uint8_t *const xram = (uint8_t *)0x30000000u;

/* Link-time code generation emits calls to these after it has decided
 * what to keep, so they must outlive their callers. */
__attribute__((used)) void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

__attribute__((used)) void *memset(void *dst, int value, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)value;
    return dst;
}

/* A misaligned base used to be the fabric's problem and it solved it with a byte
 * barrel shift in every mode engine. Now there is no shifter, so the fetch would be
 * silently wrong and look like a rendering bug. Stop where the address can be named.
 *
 * Define RP6502_XRAM_ALIGN_ERRNO to answer through the xreg's return value instead. */
bool mem_xram_align(uint16_t addr)
{
    if (!(addr & 3))
        return true;
#ifndef RP6502_XRAM_ALIGN_ERRNO
    printf("?Invalid XRAM alignment 0x%04X\n", addr);
    main_stop();
#endif
    return false;
}
