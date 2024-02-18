/*
 * Copyright (c) 2023 Brentward
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/clk.h"
#include "sys/cfg.h"
#include <time.h>
#include "hardware/rtc.h"
#include "hardware/timer.h"
#include "fatfs/ff.h"

#define CLK_ID_REALTIME 0
#define CLK_EPOCH_UNIX 1970
#define CLK_EPOCH_FAT 1980

uint64_t clk_clock_start;

void clk_init(void)
{
    rtc_init();
    datetime_t rtc_info = {
        .year = CLK_EPOCH_UNIX,
        .month = 1,
        .day = 1,
        .dotw = 5,
        .hour = 0,
        .min = 0,
        .sec = 0,
    };
    rtc_set_datetime(&rtc_info);
}

void clk_run(void)
{
    clk_clock_start = time_us_64();
}

DWORD get_fattime(void)
{
    DWORD res;
    datetime_t rtc_time;
    if (rtc_get_datetime(&rtc_time) && (rtc_time.year >= CLK_EPOCH_FAT))
    {
        res = (((DWORD)rtc_time.year - CLK_EPOCH_FAT) << 25) |
              ((DWORD)rtc_time.month << 21) |
              ((DWORD)rtc_time.day << 16) |
              (WORD)(rtc_time.hour << 11) |
              (WORD)(rtc_time.min << 5) |
              (WORD)(rtc_time.sec >> 1);
    }
    else
    {
        res = ((DWORD)(0) << 25 | (DWORD)1 << 21 | (DWORD)1 << 16);
    }
    return res;
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
        uint32_t sec = 1;
        int32_t nsec = 0;
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
        datetime_t rtc_info;
        if (!rtc_get_datetime(&rtc_info))
            return api_return_errno(API_EUNKNOWN);
        struct tm timeinfo = {
            .tm_year = rtc_info.year - 1900,
            .tm_mon = rtc_info.month - 1,
            .tm_mday = rtc_info.day,
            .tm_hour = rtc_info.hour,
            .tm_min = rtc_info.min,
            .tm_sec = rtc_info.sec,
            .tm_isdst = -1,
        };
        time_t rawtime = mktime(&timeinfo);
        int32_t rawtime_nsec = 0;
        if (!api_push_int32(&rawtime_nsec) ||
            !api_push_uint32((uint32_t *)&rawtime))
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
        time_t rawtime;
        int32_t rawtime_nsec;
        if (!api_pop_uint32((uint32_t *)&rawtime) ||
            !api_pop_int32_end(&rawtime_nsec))
            return api_return_errno(API_EINVAL);
        struct tm timeinfo = *gmtime(&rawtime);
        datetime_t rtc_info = {
            .year = timeinfo.tm_year + 1900,
            .month = timeinfo.tm_mon + 1,
            .day = timeinfo.tm_mday,
            .dotw = timeinfo.tm_wday,
            .hour = timeinfo.tm_hour,
            .min = timeinfo.tm_min,
            .sec = timeinfo.tm_sec,
        };
        if (!rtc_set_datetime(&rtc_info))
            return api_return_errno(API_EUNKNOWN);

        else
            return api_return_ax(0);
    }
    else
        return api_return_errno(API_EINVAL);
}
