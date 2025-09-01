/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <tusb.h>
#include "sys/com.h"
#include "usb/cdc.h"

static absolute_time_t break_timer;
static absolute_time_t faux_break_timer;
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

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *p_line_coding)
{
    (void)itf;
    (void)p_line_coding;
    // TODO remove this hack after 31-DEC-2025
    // TinyUSB used to have a bug where it didn't set the bit for supporting
    // breaks. Windows ignores the bit. MacOS requires the bit. And Linux
    // may or may not require the bit, newer kernels require the bit.
    // A common workaround is to drop the baud rate significantly so a
    // bit sequence of zeros will look like a break. Our implementation has
    // strict requirements of sending a full byte of zeros within 100ms
    // of changing the baud rate to 1200. e.g.
    // stty -F /dev/ttyACM1 1200 && echo -ne '\0' > /dev/ttyACM1
    // This workaround is no longer needed.
    if (p_line_coding->bit_rate == 1200)
        faux_break_timer = make_timeout_time_ms(100);
}

void cdc_task(void)
{

    if (is_breaking && absolute_time_diff_us(get_absolute_time(), break_timer) < 0)
    {
        is_breaking = false;
        com_set_uart_break(false);
    }

    if (!tud_cdc_connected())
    {
        // flush stdout to null
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
            if (absolute_time_diff_us(get_absolute_time(), faux_break_timer) > 0)
                for (size_t i = 0; i < data_len; i++)
                {
                    char ch = read_buf[i];
                    if (ch)
                        com_in_write(ch);
                    else
                    {
                        faux_break_timer = make_timeout_time_ms(0);
                        send_break_ms(10);
                    }
                }
            else
                for (size_t i = 0; i < data_len; i++)
                    com_in_write(read_buf[i]);
        }
    }
}
