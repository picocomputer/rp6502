/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RTL_SW_SHIM_PICO_TIME_H_
#define _RTL_SW_SHIM_PICO_TIME_H_

#include <stdint.h>
#include <stdbool.h>

uint64_t time_us_64(void);

typedef uint64_t absolute_time_t;

static inline absolute_time_t make_timeout_time_us(int64_t us)
{
    return time_us_64() + (us < 0 ? 0 : (uint64_t)us);
}

static inline absolute_time_t make_timeout_time_ms(int64_t ms)
{
    return time_us_64() + (ms < 0 ? 0 : (uint64_t)ms * 1000);
}

static inline bool time_reached(absolute_time_t t)
{
    return time_us_64() >= t;
}

#endif
