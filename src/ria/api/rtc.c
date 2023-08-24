#include "sys/rtc.h"
#include <stdio.h>
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

void rtc_print()
{
    char datetime_buf[256];
    char *datetime_str = &datetime_buf[0];

    datetime_t t = {
            .year  = 2020,
            .month = 06,
            .day   = 05,
            .dotw  = 5, // 0 is Sunday, so 5 is Friday
            .hour  = 15,
            .min   = 45,
            .sec   = 00
    };
    rtc_get_datetime(&t);
    datetime_to_str(datetime_str, sizeof(datetime_buf), &t);
    printf("\r%s    ?!\n", datetime_str);
}

void rtc_api_read_rtc_time(void)
{
    uint8_t *buf;
    datetime_t t = {
            .year  = 1978,
            .month = 08,
            .day   = 30,
            .dotw  = 3,
            .hour  = 19,
            .min   = 36,
            .sec   = 00
    };
    uint8_t count = sizeof(datetime_t);
    buf = &xstack[XSTACK_SIZE - count];

    if (xstack_ptr != XSTACK_SIZE)
        goto err_param;
    datetime_t t = {
            .year  = 2020,
            .month = 06,
            .day   = 05,
            .dotw  = 5, // 0 is Sunday, so 5 is Friday
            .hour  = 15,
            .min   = 45,
            .sec   = 00
    };
    rtc_get_datetime(&t);

    if (fresult == FR_OK)
        api_set_ax(br);
    else
    {
        API_ERRNO = fresult;
        api_set_ax(-1);
    }
    if (is_xram)
        std_count = br;
    else
    {
        if (br == count)
            xstack_ptr = XSTACK_SIZE - count;
        else // short reads need to be moved
            for (UINT i = br; i;)
                xstack[--xstack_ptr] = buf[--i];
        api_sync_xstack();
        api_return_released();
    }
    return;

err_param:
    xstack_ptr = XSTACK_SIZE;
    api_return_errno_axsreg_zxstack(FR_INVALID_PARAMETER, -1);

}

void rtc_api_write_rtc_time(void)
{

}