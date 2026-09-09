/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/api.h"
#include "core/api/clk.h"
#include "core/api/tim.h"
#include "host/host.h"
#include <string.h>
#include <time.h>

#define CLK_ID_REALTIME 0

static uint64_t clk_start_us;

void clk_run(void)
{
    clk_start_us = host_clock_us();
}

void clk_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u64(c, clk_start_us);
}

bool clk_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint64_t at = sst_get_u64(c);
    if (!sst_ok(c))
        return false;
    clk_start_us = at;
    return true;
}

uint32_t clk_get_run(uint32_t us_per_tick)
{
    return (host_clock_us() - clk_start_us) / us_per_tick;
}

bool clk_api_time_get(void)
{
    struct timespec ts;
    if (!tim_get_time(&ts))
        return api_return_errno(API_EIO);
    int64_t sec = ts.tv_sec;
    if (!api_push_n(&sec, sizeof(sec)))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

bool clk_api_time_set(void)
{
    uint64_t u;
    if (!api_pop_uint64_end(&u))
        return api_return_errno(API_EINVAL);
    struct timespec ts = {.tv_sec = (int64_t)u, .tv_nsec = 0};
    /* The errno is EACCES rather than ERANGE because a machine without a
     * time-of-day clock of its own is refusing to move the clock, not failing
     * to represent the value. */
    if (!tim_set_time(&ts))
        return api_return_errno(API_EACCES);
    return api_return_ax(0);
}

static bool clk_api_to_tm(bool local)
{
    uint64_t u;
    if (!api_pop_uint64_end(&u))
        return api_return_errno(API_EINVAL);
    // A short push is zero filled, so only a full eight-byte push is negative.
    time_t t = (int64_t)u;
    struct tm tm;
    if (!(local ? tim_localtime(t, &tm) : tim_gmtime(t, &tm)))
        return api_return_errno(API_EINVAL);
    if (tm.tm_year < INT16_MIN || tm.tm_year > INT16_MAX)
        return api_return_errno(API_ERANGE);
    struct clk_wire_tm w;
    clk_tm_to_wire(&tm, &w);
    if (!api_push_n(&w, sizeof(w)))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

bool clk_api_gmtime(void)
{
    return clk_api_to_tm(false);
}

bool clk_api_localtime(void)
{
    return clk_api_to_tm(true);
}

bool clk_api_mktime(void)
{
    struct clk_wire_tm w;
    if (!api_pop_n(&w, sizeof(w)) || xstack_ptr != XSTACK_SIZE)
        return api_return_errno(API_EINVAL);
    struct tm tm;
    clk_wire_to_tm(&w, &tm);
    time_t t = mktime(&tm);
    if (t == (time_t)-1)
        return api_return_errno(API_ERANGE);
    int64_t sec = t;
    if (!api_push_n(&sec, sizeof(sec)))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

bool clk_api_strftime(void)
{
    const char *format = (char *)&xstack[xstack_ptr];
    // The result is composed below the format string so the two never overlap.
    size_t max = xstack_ptr;
    /* The byte at XSTACK_SIZE is always zero, so strlen stops even when the
     * 6502 pushed no terminator. The length is checked before xstack_ptr
     * moves, because the RIA serves the 6502's XSTACK register against
     * xstack_ptr and would read past the array if it ever went above
     * XSTACK_SIZE. */
    size_t format_size = strlen(format) + 1;
    if (format_size > XSTACK_SIZE - max)
        return api_return_errno(API_EINVAL);
    xstack_ptr = max + format_size;
    struct clk_wire_tm w;
    if (!api_pop_n(&w, sizeof(w)) || xstack_ptr != XSTACK_SIZE)
        return api_return_errno(API_EINVAL);
    if (w.tm_sec < 0 || w.tm_sec > 61 || w.tm_min < 0 || w.tm_min > 59 ||
        w.tm_hour < 0 || w.tm_hour > 23 || w.tm_mday < 1 || w.tm_mday > 31 ||
        w.tm_mon < 0 || w.tm_mon > 11 || w.tm_wday < 0 || w.tm_wday > 6 ||
        w.tm_yday < 0 || w.tm_yday > 365)
        return api_return_errno(API_EINVAL);
    struct tm tm;
    clk_wire_to_tm(&w, &tm);
    size_t n = tim_strftime((char *)xstack, max, format, &tm);
    // The 6502 pops from the top of the xstack, so the result moves there.
    xstack_ptr = XSTACK_SIZE - n;
    memmove(&xstack[xstack_ptr], xstack, n);
    return api_return_ax(n);
}

// Ops 0x0F-0x12 below are retained for binaries built with older SDKs.

bool clk_api_clock(void)
{
    return api_return_axsreg(clk_get_run(10000));
}

bool clk_api_get_res(void)
{
    if (API_A != CLK_ID_REALTIME)
        return api_return_errno(API_EINVAL);
    struct timespec ts;
    tim_get_time_res(&ts);
    int32_t nsec = ts.tv_nsec;
    uint32_t sec = ts.tv_sec;
    if (!api_push_int32(&nsec) ||
        !api_push_uint32(&sec))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

bool clk_api_get_time(void)
{
    if (API_A != CLK_ID_REALTIME)
        return api_return_errno(API_EINVAL);
    struct timespec ts;
    if (!tim_get_time(&ts))
        return api_return_errno(API_EIO);
    int32_t nsec = ts.tv_nsec;
    uint32_t sec = ts.tv_sec;
    if (!api_push_int32(&nsec) ||
        !api_push_uint32(&sec))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

bool clk_api_set_time(void)
{
    if (API_A != CLK_ID_REALTIME)
        return api_return_errno(API_EINVAL);
    uint32_t rawtime_sec;
    int32_t rawtime_nsec;
    if (!api_pop_uint32(&rawtime_sec) ||
        !api_pop_int32_end(&rawtime_nsec))
        return api_return_errno(API_EINVAL);
    if (rawtime_nsec < 0 || rawtime_nsec > 999999999)
        return api_return_errno(API_EINVAL);
    struct timespec ts;
    ts.tv_sec = rawtime_sec;
    ts.tv_nsec = rawtime_nsec;
    if (!tim_set_time(&ts))
        return api_return_errno(API_EACCES);
    return api_return_ax(0);
}
