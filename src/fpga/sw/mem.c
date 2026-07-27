/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pocket port of ria/sys/mem.c, so far as the API needs it. The regs
 * symbol is the hardware register window, placed by the linker script; the
 * xstack is plain firmware memory until its RTL port arrives.
 *
 * memcpy and memset live here while the build is freestanding; picolibc
 * takes over when printf territory arrives.
 */

#include "ria/sys/mem.h"

uint8_t xstack[XSTACK_SIZE + 1];
volatile size_t xstack_ptr;

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)value;
    return dst;
}
