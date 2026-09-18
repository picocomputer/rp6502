/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/xram.h"
#include <stddef.h>

volatile uint8_t *const xram = (uint8_t *)0x30000000u;

/* Link-time code generation emits calls to memcpy and memset after
 * unreferenced functions have been discarded, so the used attribute keeps
 * both in the image. */
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
