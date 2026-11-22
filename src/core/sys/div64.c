/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/div64.h"
#include "machine.h"
#include <stddef.h>

uint64_t HOST_IN_RAM_FUNC div64(uint64_t n, uint32_t d, uint32_t *rem)
{
    uint32_t hi = (uint32_t)(n >> 32), lo = (uint32_t)n;
    /* d is an odd number times a power of two. The power of two is a
     * shift, and what it shifts out is the low bits of the remainder. */
    unsigned shift = 0;
    while (!(d & 1))
        d >>= 1, shift++;
    const uint32_t out = shift ? lo & ((1u << shift) - 1) : 0;
    if (shift)
    {
        lo = lo >> shift | hi << (32 - shift);
        hi >>= shift;
    }
    uint32_t q_hi, q_lo, r;
    if (d < 0x10000)
    {
        /* Long division, 16 bits at a time: a remainder under d and the
         * next 16 bits are under d * 65536, which is in 32 bits. */
        uint32_t part = hi >> 16;
        const uint32_t q3 = part / d;
        part = (part % d) << 16 | (hi & 0xFFFF);
        const uint32_t q2 = part / d;
        part = (part % d) << 16 | lo >> 16;
        const uint32_t q1 = part / d;
        part = (part % d) << 16 | (lo & 0xFFFF);
        q_hi = q3 << 16 | q2;
        q_lo = q1 << 16 | part / d;
        r = part % d;
    }
    else
    {
        /* The high word in one divide, then a bit at a time. The bit that
         * a shift pushes out of the remainder still counts. */
        q_hi = hi / d;
        r = hi % d;
        q_lo = 0;
        for (int i = 31; i >= 0; i--)
        {
            const uint32_t carry = r >> 31;
            r = r << 1 | (lo >> i & 1);
            if (carry || r >= d)
            {
                r -= d;
                q_lo |= 1u << i;
            }
        }
    }
    if (rem)
        *rem = r << shift | out;
    return (uint64_t)q_hi << 32 | q_lo;
}

uint32_t HOST_IN_RAM_FUNC div64_small(uint64_t n, uint64_t d, unsigned bits)
{
    /* n >> b is at least d just when d << b is no more than n, and says so
     * without the shift left ever leaving 64 bits. */
    if ((n >> bits) >= d)
        return (1u << bits) - 1;
    uint32_t q = 0;
    while (bits--)
        if ((n >> bits) >= d)
        {
            n -= d << bits;
            q |= 1u << bits;
        }
    return q;
}
