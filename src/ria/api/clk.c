/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// The original RP2040 RTC implementation by Brentward is here:
// https://github.com/picocomputer/rp6502/blob/bd8e3197/src/ria/api/clk.c

#include "api/api.h"
#include "api/clk.h"
#include "sys/cfg.h"
#include <hardware/timer.h>
#include <pico/aon_timer.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_CLK)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define CLK_ID_REALTIME 0

static uint64_t clk_clock_start;

void clk_init(void)
{
    // starting at noon avoids time zone wraparound
    const struct timespec ts = {43200, 0};
    aon_timer_start(&ts);
    cfg_set_time_zone(clk_set_time_zone(cfg_get_time_zone()));
}

void clk_run(void)
{
    clk_clock_start = time_us_64();
}

void clk_print_status(void)
{
    printf("Time: ");
    struct timespec ts;
    if (!aon_timer_get_time(&ts))
    {
        puts("get time failure");
    }
    else
    {
        char buf[100];
        struct tm tminfo;
        localtime_r(&ts.tv_sec, &tminfo);
        strftime(buf, sizeof(buf), "%c %z %Z", &tminfo);
        printf("%s\n", buf);
    }
}

const char *clk_set_time_zone(const char *tz)
{
    const char *time_zone = "UTC0";
    if (strlen(tz))
        time_zone = tz;
    setenv("TZ", time_zone, 1);
    tzset();
    return time_zone;
}

bool clk_api_clock(void)
{
    return api_return_axsreg((time_us_64() - clk_clock_start) / 10000);
}

bool clk_api_get_res(void)
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
        return api_return_ax(0);
    }
    else
        return api_return_errno(API_EINVAL);
}

bool clk_api_get_time(void)
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
        return api_return_ax(0);
    }
    else
        return api_return_errno(API_EINVAL);
}

bool clk_api_set_time(void)
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
            return api_return_errno(API_ERANGE);
        else
            return api_return_ax(0);
    }
    else
        return api_return_errno(API_EINVAL);
}

bool clk_api_get_time_zone(void)
{
    struct __attribute__((packed)) cc65_timezone
    {
        int8_t daylight;  /* True if daylight savings time active */
        int32_t timezone; /* Number of seconds behind UTC */
        char tzname[5];   /* Name of timezone, e.g. CET */
        char dstname[5];  /* Name when daylight true, e.g. CEST */
    } tz;
    static_assert(15 == sizeof(tz));

    uint8_t clock_id = API_A;
    uint32_t requested_time;
    api_pop_uint32_end(&requested_time);
    if (clock_id != CLK_ID_REALTIME)
        return api_return_errno(API_EINVAL);

    struct timespec ts;
    ts.tv_sec = requested_time;
    ts.tv_nsec = 0;

    struct tm local_tm = *localtime(&ts.tv_sec);
    struct tm gm_tm = *gmtime(&ts.tv_sec);
    gm_tm.tm_isdst = local_tm.tm_isdst; // This can't be right
    time_t local_sec = mktime(&local_tm);
    time_t gm_sec = mktime(&gm_tm);

    tz.daylight = local_tm.tm_isdst;
    tz.timezone = (int32_t)difftime(local_sec, gm_sec);
    strncpy(tz.tzname, tzname[0], 4);
    tz.tzname[4] = '\0';
    strncpy(tz.dstname, tzname[1], 4);
    tz.dstname[4] = '\0';

    for (size_t i = sizeof(tz); i;)
        if (!api_push_uint8(&(((uint8_t *)&tz)[--i])))
            return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}
