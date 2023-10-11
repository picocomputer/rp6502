#include "api/api.h"
#include "api/rtc.h"
#include "sys/cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include "hardware/rtc.h"
#include "pico/stdlib.h"

void rtc_init_ (void)
{
    rtc_init();
    datetime_t rtc_info = {
        .year = 1970,
        .month = 1,
        .day = 1,
        .dotw = 4,
        .hour = 0,
        .min = 0,
        .sec = 0,
    };
    rtc_set_datetime(&rtc_info);
    set_timezone(cfg_get_timezone());

    // set_timezone();
}

DWORD get_fattime (void)
{
    DWORD res;
    datetime_t rtc_time;
    bool rtc_set = rtc_get_datetime(&rtc_time);
    if (rtc_set && (rtc_time.year >= FAT_EPOCH_YEAR))
    {
        res =  (((DWORD)rtc_time.year - 1980) << 25)
                | ((DWORD)rtc_time.month << 21)
                | ((DWORD)rtc_time.day << 16)
                | (WORD)(rtc_time.hour << 11)
                | (WORD)(rtc_time.min << 5)
                | (WORD)(rtc_time.sec >> 1);

    } else
    {
        res =  ((DWORD)(0) << 25 
                | (DWORD)1 << 21
                | (DWORD)1 << 16);
    }
    return res;
}

void rtc_api_get_time(void)
{
    datetime_t rtc_info;
    bool result = rtc_get_datetime(&rtc_info);
    if (!result)
        api_return_errno_axsreg(7, -1);
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
    __tzinfo_type tz = *__gettzinfo();
    if (timeinfo.tm_isdst == 1)
        rawtime -= tz.__tzrule[1].offset;
    else
        rawtime -= tz.__tzrule[0].offset;
    rtc_api_get_timezone();
    api_return_axsreg((uint32_t)rawtime);
}

void rtc_api_set_time(void)
{
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
        api_return_errno_axsreg(7, -1);
    api_return_ax(0);
}

void rtc_api_get_timezone(void)
{
    long tz_timezone = 0;
    char tz_tzname[5];
    char tz_dstname[5];
    datetime_t rtc_info;
    bool result = rtc_get_datetime(&rtc_info);
    if (!result)
        api_return_errno_axsreg(7, -1); // cheeck error code
    struct tm timeinfo = {
        .tm_year = rtc_info.year - 1900,
        .tm_mon = rtc_info.month - 1,
        .tm_mday = rtc_info.day,
        .tm_hour = rtc_info.hour,
        .tm_min = rtc_info.min,
        .tm_sec = rtc_info.sec,
        .tm_isdst = -1,
    };
    mktime(&timeinfo);
    __tzinfo_type tz = *__gettzinfo();
    printf("timeinfo: %d-%d-%d %d:%d:%d, dotw: %d, isdst: %d\n", timeinfo.tm_year, timeinfo.tm_mon, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, timeinfo.tm_wday,  timeinfo.tm_isdst);
    char tz_daylight = (char)timeinfo.tm_isdst;
    printf("offset 0: %ld\n", tz.__tzrule[0].offset);
    printf("offset 1: %ld\n", tz.__tzrule[1].offset);
    if (timeinfo.tm_isdst == 1) {
        tz_timezone -= tz.__tzrule[1].offset;
        strlcpy(tz_tzname, _tzname[1], sizeof(tz_tzname));
        strlcpy(tz_dstname, _tzname[1], sizeof(tz_dstname));
    } else
    {
        tz_timezone -= tz.__tzrule[0].offset;
        strlcpy(tz_tzname, _tzname[0], sizeof(tz_tzname));
        strlcpy(tz_dstname, _tzname[1], sizeof(tz_dstname));
    }
    printf("tz_daylight: %d\n", tz_daylight);
    printf("tz_timezone: %ld\n", tz_timezone);
    printf("tz_tzname: %s\n", tz_tzname);
    printf("tz_dstname: %s\n", tz_dstname);
}

void set_timezone(const char *timezone)
{
    setenv ("TZ", timezone, 1);
    tzset ();
    // __tzset_parse_tz
}