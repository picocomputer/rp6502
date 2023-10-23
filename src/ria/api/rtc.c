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
    bool running;
    running = rtc_running();
    if (running == false) {
        rtc_init();
        datetime_t rtc_info = {
            .year = 1970,
            .month = 1,
            .day = 2,
            .dotw = 5,
            .hour = 0,
            .min = 0,
            .sec = 0,
        };
        rtc_set_datetime(&rtc_info);
        set_timezone(cfg_get_timezone());
    }
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

void rtc_api_get_res(void)
{
    uint8_t clock_id = API_A;
    if (clock_id == 0)
    {
        uint32_t sec = 1;
        int32_t nsec = 0;
        api_push_int32(&nsec);
        api_push_uint32(&sec);
        api_sync_xstack();
        return api_return_ax(0);
    }
    else
        return api_return_errno(API_EINVAL);
}


void rtc_api_get_time(void)
{
    uint8_t clock_id = API_A;
    if (clock_id == 0) {
        datetime_t rtc_info;
        bool result = rtc_get_datetime(&rtc_info);
        if (!result)
            return api_return_errno(API_EINVAL);
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
        __tzinfo_type tz = *__gettzinfo();
        if (timeinfo.tm_isdst == 1)
            rawtime -= tz.__tzrule[1].offset;
        else
            rawtime -= tz.__tzrule[0].offset;
        api_push_int32(&rawtime_nsec);
        api_push_uint32((uint32_t *)&rawtime);
        api_sync_xstack();
        return api_return_ax(0);
    }
    else
        return api_return_errno(API_EINVAL);
}

void rtc_api_set_time(void)
{
    uint8_t clock_id = API_A;
    if (clock_id == 0) {
        time_t rawtime;
        long rawtime_nsec;

        api_pop_uint32((uint32_t *)&rawtime);
        api_pop_int32(&rawtime_nsec);
        api_sync_xstack();
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
            return api_return_errno(API_EINVAL);

        else
            return api_return_ax(0);
    }
    else
        return api_return_errno(API_EINVAL);
}

void rtc_api_get_timezone(void)
{
    uint8_t clock_id = API_A;
    if (clock_id == 0) {
        struct ria_timezone {
            char    daylight;   /* True if daylight savings time active */
            long    timezone;   /* Number of seconds behind UTC */
            char    tz_name[5];  /* Name of timezone, e.g. CET */
            char    dstname[5]; /* Name when daylight true, e.g. CEST */
        } ria_tz;
        ria_tz.timezone = 0;
        datetime_t rtc_info;
        bool result = rtc_get_datetime(&rtc_info);
        if (!result)
            return api_return_errno(API_EINVAL); // cheeck error code
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
        ria_tz.daylight = (char)timeinfo.tm_isdst;
        if (timeinfo.tm_isdst == 1) {
            ria_tz.timezone -= tz.__tzrule[1].offset;
            strlcpy(ria_tz.tz_name, _tzname[1], sizeof(ria_tz.tz_name));
            strlcpy(ria_tz.dstname, _tzname[1], sizeof(ria_tz.dstname));
        } else {
            ria_tz.timezone -= tz.__tzrule[0].offset;
            strlcpy(ria_tz.tz_name, _tzname[0], sizeof(ria_tz.tz_name));
            strlcpy(ria_tz.dstname, _tzname[1], sizeof(ria_tz.dstname));
        }
        uint8_t i;
        for (i = 0; i < 5; i ++)
        {
            api_push_uint8((const uint8_t *)&(ria_tz.dstname[4 - i]));
        }
        for (i = 0; i < 5; i ++)
        {
            api_push_uint8((const uint8_t *)&(ria_tz.tz_name[4 - i]));
        }
        api_push_int32(&(ria_tz.timezone));
        api_push_uint8((const uint8_t *)&(ria_tz.daylight));
        api_sync_xstack();
        return api_return_ax(0);

    }
    else
        return api_return_errno(API_EINVAL);

}

void set_timezone(const char *timezone)
{
    setenv ("TZ", timezone, 1);
    tzset ();
}