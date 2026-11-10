/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TEST_PICO_TIME_H_
#define _TEST_PICO_TIME_H_

/* The Pico SDK's time API on the test's clock, in microseconds. */

#include <stdbool.h>
#include <stdint.h>

typedef uint64_t absolute_time_t;

absolute_time_t get_absolute_time(void);

static inline uint64_t time_us_64(void)
{
    return get_absolute_time();
}

static inline absolute_time_t delayed_by_ms(absolute_time_t t, uint32_t ms)
{
    return t + (uint64_t)ms * 1000;
}

static inline absolute_time_t make_timeout_time_ms(uint32_t ms)
{
    return delayed_by_ms(get_absolute_time(), ms);
}

static inline bool time_reached(absolute_time_t t)
{
    return get_absolute_time() >= t;
}

static inline int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to)
{
    return (int64_t)(to - from);
}

#endif /* _TEST_PICO_TIME_H_ */
