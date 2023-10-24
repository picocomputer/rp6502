#include "api/api.h"
#include "api/rtc.h"
#include "sys/cfg.h"
#include <time.h>
#include "hardware/rtc.h"

#define RIA_CLOCK_REALTIME 0
#define FAT_EPOCH_YEAR 1980

void rtc_init_ (void)
{
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
    if (clock_id == RIA_CLOCK_REALTIME)
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
    if (clock_id == RIA_CLOCK_REALTIME) {
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
    if (clock_id == RIA_CLOCK_REALTIME) {
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
