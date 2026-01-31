/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <tusb.h>
#include "sys/com.h"
#include "usb/cdc.h"

static absolute_time_t break_timer;
static bool is_breaking = false;
static uint8_t read_buf[COM_IN_BUF_SIZE];

static void send_break_ms(uint16_t duration_ms)
{
    break_timer = make_timeout_time_ms(duration_ms);
    is_breaking = true;
    com_set_uart_break(true);
}

void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms)
{
    (void)itf;
    (void)duration_ms;
    send_break_ms(duration_ms);
}

void cdc_task(void)
{
    if (is_breaking && absolute_time_diff_us(get_absolute_time(), break_timer) < 0)
    {
        is_breaking = false;
        com_set_uart_break(false);
    }

    if (!tud_cdc_connected() || !tud_cdc_write_available())
    {
        // flush to null
        while (!com_out_empty())
            com_out_read();
    }
    else
    {
        if (!com_out_empty())
        {
            while (!com_out_empty() && tud_cdc_write_char(com_out_peek()))
                com_out_read();
            tud_cdc_write_flush();
        }
        if (tud_cdc_available())
        {
            size_t bufsize = com_in_free();
            size_t data_len = tud_cdc_read(read_buf, bufsize);
            for (size_t i = 0; i < data_len; i++)
                com_in_write(read_buf[i]);
        }
    }
}
