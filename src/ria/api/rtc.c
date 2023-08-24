#include "api/api.h"
#include "api/rtc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "hardware/rtc.h"
#include "pico/stdlib.h"
#include "pico/util/datetime.h"

#define RTC_SET 0
#define RTC_NOT_SET 1
#define RTC_NTC_PENDING 2

// void rtc_init_()
// {
//     datetime_t t = {
//             .year  = 2022,
//             .month = 8,
//             .day   = 30,
//             .dotw  = 3,
//             .hour  = 19,
//             .min   = 36,
//             .sec   = 00
//     };
//     rtc_init();
//     rtc_set_datetime(&t);
// }

DWORD get_fattime (void)
{
    DWORD res;
    datetime_t datetime;
    bool rtc_set;

    rtc_set = rtc_get_datetime(&datetime);

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
    datetime_t *datetime;
    uint8_t count = sizeof(datetime_t);
    bool result;
    datetime = (datetime_t *)(u_int8_t *)&(xstack[XSTACK_SIZE - count]);
    if (xstack_ptr != XSTACK_SIZE)
        goto err_param;
    result = rtc_get_datetime(datetime);
    if (result)
        api_set_ax(0);
    else
        goto err_param;
    xstack_ptr = XSTACK_SIZE - count;
    api_sync_xstack();
    api_return_released();
    return;

err_param:
    xstack_ptr = XSTACK_SIZE;
    api_return_errno_axsreg_zxstack(1, -1);
}

void rtc_api_set_time(void)
{
    datetime_t *datetime;
    bool result;
    datetime = (datetime_t *)(u_int8_t *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    if (xstack_ptr != XSTACK_SIZE)
        goto err_param;
    rtc_init();
    result = rtc_set_datetime(datetime);
    if (!result)
        return api_return_errno_ax(RTC_NOT_SET, -1);
    return api_return_ax(RTC_SET);

err_param:
    xstack_ptr = XSTACK_SIZE;
    api_return_errno_axsreg_zxstack(1, -1);
}
