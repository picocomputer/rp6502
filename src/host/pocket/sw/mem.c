/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stddef.h>
#include <stdint.h>

/* A word of memory, which may hold anything. */
typedef uint32_t mem_word_t __attribute__((may_alias));

/* True when a word access at p moves four bytes: the cached memory at 0,
 * the video memory at 0x5 and the staging store at 0x6. SRAM and XRAM_WIN
 * answer a word access with one byte, and the other windows are
 * registers, so they are copied a byte at a time. In the staging store a
 * word costs what a byte does, and a word written there is two of the
 * SDRAM's writes where four bytes are four. */
static inline int mem_wide(const void *p)
{
    return (0x61u >> ((uintptr_t)p >> 28)) & 1u;
}

/* Link-time code generation emits calls to memcpy and memset after
 * unreferenced functions have been discarded, so the used attribute keeps
 * both in the image. Once the destination is on a word, a source on
 * another byte of a word is read a word at a time and shifted into
 * place, reading only words that hold bytes it copies. */
__attribute__((used)) void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (n >= 8 && mem_wide(d) && mem_wide(s))
    {
        for (; (uintptr_t)d & 3u; n--)
            *d++ = *s++;
        mem_word_t *dw = (mem_word_t *)d;
        unsigned k = (uintptr_t)s & 3u;
        const mem_word_t *sw = (const mem_word_t *)(s - k);
        if (!k)
            for (; n >= 4; n -= 4)
                *dw++ = *sw++;
        else
        {
            unsigned lo = 8 * k, hi = 32 - lo;
            uint32_t w = *sw++;
            for (; n >= 4; n -= 4)
            {
                uint32_t x = *sw++;
                *dw++ = w >> lo | x << hi;
                w = x;
            }
        }
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw - (k ? 4 - k : 0);
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

__attribute__((used)) void *memset(void *dst, int value, size_t n)
{
    uint8_t *d = dst;
    if (n >= 8 && mem_wide(d))
    {
        for (; (uintptr_t)d & 3u; n--)
            *d++ = (uint8_t)value;
        uint32_t w = (uint8_t)value * 0x01010101u;
        mem_word_t *dw = (mem_word_t *)d;
        for (; n >= 4; n -= 4)
            *dw++ = w;
        d = (uint8_t *)dw;
    }
    while (n--)
        *d++ = (uint8_t)value;
    return dst;
}
