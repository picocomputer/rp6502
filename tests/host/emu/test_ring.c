/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/ring.h"
#include "utest.h"

typedef RING(uint16_t, 8) ring8_t;

/* Both counters at n, an empty ring that wraps 2^32 after 2^32 - n entries. */
static void ring_start(ring8_t *r, uint32_t n)
{
    atomic_store(&r->idx.wr, n);
    atomic_store(&r->idx.rd, n);
}

UTEST(ring, every_slot_holds_an_entry)
{
    static ring8_t r;
    for (uint16_t i = 0; i < 8; i++)
        ASSERT_EQ(RING_PUT(&r, (uint16_t)(100 + i)), 1);
    ASSERT_EQ(RING_LEN(&r), 8u);
    ASSERT_EQ(RING_AVAIL(&r), 0u);
    ASSERT_EQ(RING_PUT(&r, (uint16_t)999), 0);
    uint16_t v;
    for (uint16_t i = 0; i < 8; i++)
    {
        ASSERT_EQ(RING_GET(&r, &v), 1);
        ASSERT_EQ(v, (uint16_t)(100 + i));
    }
    ASSERT_EQ(RING_GET(&r, &v), 0);
    ASSERT_EQ(RING_LEN(&r), 0u);
}

UTEST(ring, counters_wrap_past_2_to_the_32)
{
    static ring8_t r;
    ring_start(&r, 0xFFFFFFFDu);
    for (uint16_t i = 0; i < 8; i++)
        ASSERT_EQ(RING_PUT(&r, i), 1);
    ASSERT_EQ(RING_LEN(&r), 8u);
    ASSERT_EQ(RING_PUT(&r, (uint16_t)8), 0);
    uint16_t v;
    for (uint16_t i = 0; i < 8; i++)
    {
        ASSERT_EQ(RING_GET(&r, &v), 1);
        ASSERT_EQ(v, i);
    }
    ASSERT_EQ(atomic_load(&r.idx.rd), 5u);
    ASSERT_EQ(RING_LEN(&r), 0u);
}

UTEST(ring, in_and_out_split_at_the_end_of_the_array)
{
    static ring8_t r;
    ring_start(&r, 5);
    const uint16_t src[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    ASSERT_EQ(RING_IN(&r, src, 10), 8u);
    ASSERT_EQ(RING_IN(&r, src, 1), 0u);
    ASSERT_EQ(RING_LINEAR(&r), 3u);
    ASSERT_EQ(*RING_AT(&r, 0), 1);
    ASSERT_EQ(*RING_AT(&r, 3), 4);
    uint16_t dst[10] = {0};
    ASSERT_EQ(RING_OUT(&r, dst, 2), 2u);
    ASSERT_EQ(RING_OUT(&r, dst + 2, 10), 6u);
    for (int i = 0; i < 8; i++)
        ASSERT_EQ(dst[i], src[i]);
    ASSERT_EQ(RING_OUT(&r, dst, 1), 0u);
}

UTEST(ring, skip_drops_the_oldest)
{
    static ring8_t r;
    ring_start(&r, 6);
    for (uint16_t i = 0; i < 5; i++)
        RING_PUT(&r, i);
    RING_SKIP(&r, 3);
    ASSERT_EQ(RING_LEN(&r), 2u);
    ASSERT_EQ(*RING_AT(&r, 0), 3);
    ASSERT_EQ(*RING_AT(&r, 1), 4);
    ASSERT_EQ(RING_LINEAR(&r), 2u);
    RING_RESET(&r);
    ASSERT_EQ(RING_LEN(&r), 0u);
    ASSERT_EQ(atomic_load(&r.idx.wr), 0u);
}

UTEST_MAIN();
