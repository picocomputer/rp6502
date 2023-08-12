/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb.h"
#include "sys/std.h"
#include "usb/cdc.h"

static absolute_time_t break_timer = {0};
static bool is_breaking = false;
static uint8_t read_buf[32];

void cdc_task(void)
{

    if (is_breaking && absolute_time_diff_us(get_absolute_time(), break_timer) < 0)
    {
        is_breaking = false;
        std_set_break(false);
    }

    if (!tud_cdc_connected())
    {
        // Not connected, flush STDOUT to null
        while (!std_out_empty())
            std_out_read();
    }
    {
        if (!std_out_empty())
        {
            while (!std_out_empty() && tud_cdc_write_char(std_out_peek()))
                std_out_read();
            tud_cdc_write_flush();
        }
        if (tud_cdc_available())
        {
            size_t bufsize = std_in_free();
            if (bufsize > sizeof(read_buf))
                bufsize = sizeof(read_buf);
            size_t data_len = tud_cdc_read(read_buf, bufsize);
            for (size_t i = 0; i < data_len; i++)
                std_in_write(read_buf[i]);
        }
    }
}

void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms)
{
    break_timer = delayed_by_us(get_absolute_time(), duration_ms * 1000);
    is_breaking = true;
    std_set_break(true);
}
