/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/api.h"
#include "core/tim/clk.h"
#include "core/sys/div64.h"
#include "core/tim/tim.h"
#include "host/host.h"
#include "machine.h"
#include <string.h>

static uint64_t clk_start_us;

#define CLK_SST(X) X(U64, clk_start_us)
SST_TABLE(clk, CLK_SST)

void clk_run(void)
{
    clk_start_us = host_clock_us();
}

uint32_t clk_get_run(uint32_t us_per_tick)
{
    return (uint32_t)div64(host_clock_us() - clk_start_us, us_per_tick, NULL);
}

bool HOST_IN_FLASH_FUNC clk_api_time_get(void)
{
    if (xstack_ptr != XSTACK_SIZE)
        return api_return_errno(API_EINVAL);
    tim_spec_t ts;
    if (!tim_get_time(&ts))
        return api_return_errno(API_EIO);
    int64_t sec = ts.sec;
    api_push_n(&sec, sizeof(sec));
    return api_return_ax(0);
}

bool HOST_IN_FLASH_FUNC clk_api_time_set(void)
{
    int64_t sec;
    if (!api_pop_int64_end(&sec))
        return api_return_errno(API_EINVAL);
    tim_spec_t ts = {.sec = sec, .nsec = 0};
    /* The errno is EACCES rather than ERANGE because a machine without a
     * time-of-day clock of its own is refusing to move the clock, not failing
     * to represent the value. */
    if (!tim_set_time(&ts))
        return api_return_errno(API_EACCES);
    return api_return_ax(0);
}

static bool clk_api_to_tm(bool local)
{
    int64_t sec;
    if (!api_pop_int64_end(&sec))
        return api_return_errno(API_EINVAL);
    tim_tm_t tm;
    if (!(local ? tim_localtime(sec, &tm) : tim_gmtime(sec, &tm)) ||
        tm.tm_year < INT16_MIN || tm.tm_year > INT16_MAX)
        return api_return_errno(API_ERANGE);
    struct clk_wire_tm w;
    clk_tm_to_wire(&tm, &w);
    api_push_n(&w, sizeof(w));
    return api_return_ax(0);
}

bool HOST_IN_FLASH_FUNC clk_api_gmtime(void)
{
    return clk_api_to_tm(false);
}

bool HOST_IN_FLASH_FUNC clk_api_localtime(void)
{
    return clk_api_to_tm(true);
}

bool HOST_IN_FLASH_FUNC clk_api_mktime(void)
{
    struct clk_wire_tm w;
    if (!api_pop_n(&w, sizeof(w)) || xstack_ptr != XSTACK_SIZE)
        return api_return_errno(API_EINVAL);
    tim_tm_t tm;
    clk_wire_to_tm(&w, &tm);
    int64_t t;
    if (!tim_mktime(&tm, &t))
        return api_return_errno(API_ERANGE);
    int64_t sec = t;
    api_push_n(&sec, sizeof(sec));
    return api_return_ax(0);
}

bool HOST_IN_FLASH_FUNC clk_api_strftime(void)
{
    // The result is composed below the format string so the two never overlap.
    size_t max = xstack_ptr;
    const char *format = api_pop_str(XSTACK_SIZE);
    struct clk_wire_tm w;
    if (!format || !api_pop_n(&w, sizeof(w)) || xstack_ptr != XSTACK_SIZE)
        return api_return_errno(API_EINVAL);
    if (w.tm_sec < 0 || w.tm_sec > 61 || w.tm_min < 0 || w.tm_min > 59 ||
        w.tm_hour < 0 || w.tm_hour > 23 || w.tm_mday < 1 || w.tm_mday > 31 ||
        w.tm_mon < 0 || w.tm_mon > 11 || w.tm_wday < 0 || w.tm_wday > 6 ||
        w.tm_yday < 0 || w.tm_yday > 365)
        return api_return_errno(API_EINVAL);
    /* A 6502 program's struct tm has no offset and no zone, so %z and %Z
     * are the machine's zone at that date, by its tm_isdst. */
    tim_tm_t tm;
    clk_wire_to_tm(&w, &tm);
    tim_tm_zone(&tm);
    size_t n = tim_strftime((char *)xstack, max, format, &tm);
    // The 6502 pops from the top of the xstack, so the result moves there.
    xstack_ptr = XSTACK_SIZE - n;
    memmove(&xstack[xstack_ptr], xstack, n);
    return api_return_ax(n);
}
