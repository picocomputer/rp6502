/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <pico/stdlib.h>
#include "tusb.h"
#include "cdc.h"

static absolute_time_t break_timer = {0};
static bool break_en = false;

void cdc_init(void)
{
    gpio_set_function(PICOPROBE_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PICOPROBE_UART_RX, GPIO_FUNC_UART);
    uart_init(PICOPROBE_UART_INTERFACE, PICOPROBE_UART_BAUDRATE);
}

void cdc_reclock(void)
{
    uart_init(PICOPROBE_UART_INTERFACE, PICOPROBE_UART_BAUDRATE);
}

#define MAX_UART_PKT 64
void cdc_task(void)
{
    uint8_t rx_buf[MAX_UART_PKT];
    uint8_t tx_buf[MAX_UART_PKT];

    if (break_en && absolute_time_diff_us(get_absolute_time(), break_timer) < 0)
    {
        break_en = false;
        uart_set_break(PICOPROBE_UART_INTERFACE, false);
    }

    // Consume uart fifo regardless even if not connected
    uint rx_len = 0;
    while (uart_is_readable(PICOPROBE_UART_INTERFACE) && (rx_len < MAX_UART_PKT))
    {
        char ch = uart_getc(PICOPROBE_UART_INTERFACE);
        putchar_raw(ch);
        rx_buf[rx_len++] = ch;
    }

    if (tud_cdc_connected())
    {
        // Do we have anything to display on the host's terminal?
        if (rx_len)
        {
            for (uint i = 0; i < rx_len; i++)
            {
                tud_cdc_write_char(rx_buf[i]);
            }
            tud_cdc_write_flush();
        }

        if (tud_cdc_available())
        {
            // Is there any data from the host for us to tx
            uint tx_len = tud_cdc_read(tx_buf, sizeof(tx_buf));
            uart_write_blocking(PICOPROBE_UART_INTERFACE, tx_buf, tx_len);
        }
    }
}

void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms)
{
    break_timer = delayed_by_us(get_absolute_time(), duration_ms * 1000);
    break_en = true;
    uart_set_break(PICOPROBE_UART_INTERFACE, true);
}
