/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/div64.h"
#include "utest.h"
#include <stddef.h>

static void check(int *utest_result, uint64_t n, uint32_t d)
{
    uint32_t rem = 0xDEADBEEF;
    ASSERT_EQ(div64(n, d, &rem), n / d);
    ASSERT_EQ((uint64_t)rem, n % d);
    ASSERT_EQ(div64(n, d, NULL), n / d);
}

/* A spread of both, by a generator, with the ends of each. */
static uint64_t next(uint64_t *state)
{
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    return *state ^ (*state >> 29);
}

UTEST(div64, every_kind_of_divisor)
{
    static const uint32_t divisors[] = {
        1, 2, 3, 10, 127, 128, 675, 1000, 10000, 49716, 65535, 65536, 65537, 86400,
        100000, 1000000, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF};
    static const uint64_t numbers[] = {
        0, 1, 2, 0xFFFF, 0x10000, 0xFFFFFFFF, 0x100000000ull, 0x7FFFFFFFFFFFFFFFull,
        0x8000000000000000ull, 0xFFFFFFFFFFFFFFFEull, 0xFFFFFFFFFFFFFFFFull};
    for (size_t i = 0; i < sizeof divisors / sizeof divisors[0]; i++)
        for (size_t j = 0; j < sizeof numbers / sizeof numbers[0]; j++)
            check(utest_result, numbers[j], divisors[i]);
    uint64_t state = 1;
    for (int i = 0; i < 200000; i++)
    {
        uint64_t n = next(&state);
        uint32_t d = (uint32_t)next(&state);
        /* Small numbers and small divisors as often as large ones. */
        n >>= next(&state) & 63;
        d >>= next(&state) & 31;
        if (d)
            check(utest_result, n, d);
    }
}

UTEST(div64, a_small_answer_from_a_wide_divisor)
{
    ASSERT_EQ(div64_small(0, 1, 15), 0u);
    ASSERT_EQ(div64_small(639, 1, 15), 639u);
    ASSERT_EQ(div64_small(32767, 1, 15), 32767u);
    /* One that does not fit is the largest that does. */
    ASSERT_EQ(div64_small(32768, 1, 15), 32767u);
    ASSERT_EQ(div64_small(0xFFFFFFFFFFFFFFFFull, 1, 15), 32767u);
    ASSERT_EQ(div64_small(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 15), 1u);
    ASSERT_EQ(div64_small(0xFFFFFFFFFFFFFFFEull, 0xFFFFFFFFFFFFFFFFull, 15), 0u);
    uint64_t state = 7;
    for (int i = 0; i < 200000; i++)
    {
        uint64_t d = next(&state) >> (next(&state) & 63);
        if (!d)
            continue;
        const uint64_t q = next(&state) & 0xFFFF;
        const uint64_t r = next(&state) % d;
        /* A product that leaves 64 bits is not one this checks. */
        if (q && d > (0xFFFFFFFFFFFFFFFFull - r) / q)
            continue;
        const uint64_t n = q * d + r;
        ASSERT_EQ((uint64_t)div64_small(n, d, 15), q > 32767 ? 32767 : q);
    }
}

UTEST_MAIN();
