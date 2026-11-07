/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/ring.h"
#include <string.h>

uint32_t ring_in(ring_t *i, void *buf, uint32_t size, size_t esize,
                 const void *src, uint32_t n)
{
    const uint32_t avail = size - ring_len(i);
    if (n > avail)
        n = avail;
    const uint32_t at = ring_wr_slot(i, size);
    const uint32_t first = n < size - at ? n : size - at;
    memcpy((char *)buf + at * esize, src, first * esize);
    memcpy(buf, (const char *)src + first * esize, (n - first) * esize);
    ring_wr_add(i, n);
    return n;
}

uint32_t ring_out(ring_t *i, const void *buf, uint32_t size, size_t esize,
                  void *dst, uint32_t n)
{
    const uint32_t len = ring_len(i);
    if (n > len)
        n = len;
    const uint32_t at = ring_rd_slot(i, 0, size);
    const uint32_t first = n < size - at ? n : size - at;
    memcpy(dst, (const char *)buf + at * esize, first * esize);
    memcpy((char *)dst + first * esize, buf, (n - first) * esize);
    ring_rd_add(i, n);
    return n;
}
