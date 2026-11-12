/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stddef.h>
#include <stdint.h>

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
