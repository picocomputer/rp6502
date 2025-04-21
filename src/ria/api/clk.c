/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// The original RP2040 RTC implementation by Brentward is here:
// https://github.com/picocomputer/rp6502/blob/bd8e3197/src/ria/api/clk.c

#include "api/api.h"
#include "api/clk.h"
#include "hardware/timer.h"
#include "pico/aon_timer.h"

#define CLK_ID_REALTIME 0

uint64_t clk_clock_start;

void clk_init(void)
{
    const struct timespec ts = {0, 0};
    aon_timer_start(&ts);
}

void clk_run(void)
{
    clk_clock_start = time_us_64();
}

void clk_api_clock(void)
{
    return api_return_axsreg((time_us_64() - clk_clock_start) / 10000);
}

void clk_api_get_res(void)
{
    uint8_t clock_id = API_A;
    if (clock_id == CLK_ID_REALTIME)
    {
        struct timespec ts;
        aon_timer_get_resolution(&ts);
        int32_t nsec = ts.tv_nsec;
        uint32_t sec = ts.tv_sec;
        if (!api_push_int32(&nsec) ||
            !api_push_uint32(&sec))
            return api_return_errno(API_EINVAL);
        api_sync_xstack();
        return api_return_ax(0);
    }
    else
        return api_return_errno(API_EINVAL);
}

void clk_api_get_time(void)
{
    uint8_t clock_id = API_A;
    if (clock_id == CLK_ID_REALTIME)
    {
        struct timespec ts;
        aon_timer_get_time(&ts);
        int32_t nsec = ts.tv_nsec;
        uint32_t sec = ts.tv_sec;
        if (!api_push_int32(&nsec) ||
            !api_push_uint32(&sec))
            return api_return_errno(API_EINVAL);
        api_sync_xstack();
        return api_return_ax(0);
    }
    else
        return api_return_errno(API_EINVAL);
}

void clk_api_set_time(void)
{
    uint8_t clock_id = API_A;
    if (clock_id == CLK_ID_REALTIME)
    {
        uint32_t rawtime_sec;
        int32_t rawtime_nsec;
        if (!api_pop_uint32(&rawtime_sec) ||
            !api_pop_int32_end(&rawtime_nsec))
            return api_return_errno(API_EINVAL);
        struct timespec ts;
        ts.tv_sec = rawtime_sec;
        ts.tv_nsec = rawtime_nsec;
        if (!aon_timer_set_time(&ts))
            return api_return_errno(API_EUNKNOWN);
        else
            return api_return_ax(0);
    }
    else
        return api_return_errno(API_EINVAL);
}
