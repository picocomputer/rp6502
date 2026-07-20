/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// The original RP2040 RTC implementation by Brentward is here:
// https://github.com/picocomputer/rp6502/blob/bd8e3197/src/ria/api/clk.c

#include "ria/api/api.h"
#include "ria/api/clk.h"
#include "ria/api/tim.h"
#include <pico/time.h>
#include <assert.h>
#include <string.h>
#include <time.h>

#define CLK_ID_REALTIME 0

static uint64_t clk_start_us;

void clk_run(void)
{
    clk_start_us = time_us_64();
}

uint32_t clk_get_run(uint32_t us_per_tick)
{
    return (time_us_64() - clk_start_us) / us_per_tick;
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
    if (!tim_set_time(&ts))
        return api_return_errno(API_ERANGE);
    return api_return_ax(0);
}

static bool clk_api_to_tm(bool local)
{
    uint64_t u;
    if (!api_pop_uint64_end(&u))
        return api_return_errno(API_EINVAL);
    // Short pushes are unsigned; 8-byte pushes carry the sign.
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
    // Compose below the format so they never overlap.
    size_t max = xstack_ptr;
    // The guard byte backstops a missing terminator. Validate before
    // advancing; core 1 serves 0xFFEC against xstack_ptr concurrently,
    // so it must never be published past XSTACK_SIZE.
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
    // relocate buffer to top of xstack
    xstack_ptr = XSTACK_SIZE - n;
    memmove(&xstack[xstack_ptr], xstack, n);
    return api_return_ax(n);
}

// Deprecated. Retained for binaries built with older SDKs.

bool clk_api_clock(void)
{
    return api_return_axsreg(clk_get_run(10000));
}

// MSVC has no __attribute__((packed)); pragma pack is the spelling every
// toolchain honors, and the wire layout is what the old SDKs expect.
#pragma pack(push, 1)
typedef struct
{
    int8_t daylight;
    int32_t timezone;
    char tzname[5];
    char dstname[5];
} clk_tz_wire_t;
#pragma pack(pop)

bool clk_api_tzset(void)
{
    clk_tz_wire_t tz;
    static_assert(15 == sizeof(tz));
    tz.daylight = tim_get_tz_daylight();
    tz.timezone = tim_get_tz_offset();
    strncpy(tz.tzname, tim_get_tz_name(false), 4);
    tz.tzname[4] = '\0';
    strncpy(tz.dstname, tim_get_tz_name(true), 4);
    tz.dstname[4] = '\0';
    if (!api_push_n(&tz, sizeof(tz)))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

bool clk_api_tzquery(void)
{
    uint32_t epoch_sec = API_AXSREG;
    struct timespec ts;
    ts.tv_sec = epoch_sec;
    ts.tv_nsec = 0;
    // mktime(localtime(t)) - mktime(gmtime(t) with local DST) yields the
    // UTC offset in seconds east of UTC.
    struct tm local_tm = *localtime(&ts.tv_sec);
    struct tm gm_tm = *gmtime(&ts.tv_sec);
    gm_tm.tm_isdst = local_tm.tm_isdst;
    time_t local_sec = mktime(&local_tm);
    time_t gm_sec = mktime(&gm_tm);
    uint8_t isdst = local_tm.tm_isdst;
    if (!api_push_uint8(&isdst))
        return api_return_errno(API_EINVAL);
    int32_t seconds = (int32_t)(local_sec - gm_sec);
    return api_return_axsreg(seconds);
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
        return api_return_errno(API_ERANGE);
    return api_return_ax(0);
}
