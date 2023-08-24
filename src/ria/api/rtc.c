#include "api/api.h"
#include "api/rtc.h"
#include <stdio.h>
#include <stdlib.h>
#include "hardware/rtc.h"
#include "pico/stdlib.h"
#include "pico/util/datetime.h"

void rtc_init_()
{
    datetime_t t = {
            .year  = 1978,
            .month = 8,
            .day   = 30,
            .dotw  = 3,
            .hour  = 19,
            .min   = 36,
            .sec   = 00
    };
    rtc_init();
    rtc_set_datetime(&t);
}

void rtc_api_get_time(void)
{
    datetime_t *datetime;
    uint8_t count = sizeof(datetime_t);
    uint8_t *buf = (uint8_t *)malloc(count);
    bool result;
    buf = &xstack[XSTACK_SIZE - count];
    datetime = (datetime_t *) buf;
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
    uint8_t count = XSTACK_SIZE - xstack_ptr;
    uint8_t *buf = (uint8_t *)malloc(count);
    bool result;
    buf = &xstack[xstack_ptr];
    datetime = (datetime_t *)buf;
    xstack_ptr = XSTACK_SIZE;
    if (xstack_ptr != XSTACK_SIZE)
        goto err_param;
    result = rtc_set_datetime(datetime);
    if (!result)
        return api_return_errno_ax(1, -1);
    return api_return_ax(0);

err_param:
    xstack_ptr = XSTACK_SIZE;
    api_return_errno_axsreg_zxstack(1, -1);
}