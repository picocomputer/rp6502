#include "api/api.h"
#include "api/rtc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include "hardware/rtc.h"
#include "pico/stdlib.h"
#include "pico/util/datetime.h"

DWORD get_fattime (void)
{
    DWORD res;
    datetime_t datetime;
    bool rtc_set = rtc_get_datetime(&datetime);
    if (rtc_set)
    {
        res =  (((DWORD)datetime.year - 1980) << 25)
                | ((DWORD)datetime.month << 21)
                | ((DWORD)datetime.day << 16)
                | (WORD)(datetime.hour << 11)
                | (WORD)(datetime.min << 5)
                | (WORD)(datetime.sec >> 1);

    } else
    {
        res =  ((DWORD)(FF_NORTC_YEAR - 1980) << 25 
                | (DWORD)FF_NORTC_MON << 21
                | (DWORD)FF_NORTC_MDAY << 16);
    }
    return res;
}

void rtc_api_get_time(void)
{
    datetime_t rtc_info;
    bool result = rtc_get_datetime(&rtc_info);
    if (!result)
        return api_return_errno_axsreg(RTC_NOT_SET, -1);
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
    api_return_axsreg((uint32_t)rawtime);
}

void rtc_api_set_time(void)
{
    if (!rtc_running())
        rtc_init();
    time_t rawtime = (time_t)API_AXSREG;
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
    bool result = rtc_set_datetime(&rtc_info);
    if (!result)
        return api_return_errno_axsreg(RTC_INVALID_DATETIME, -1);
    return api_return_ax(RTC_OK);
}