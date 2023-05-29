/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cfg.h"
#include "com.h"
#include "cpu.h"
#include "main.h"
#include "mem.h"
#include "ria.h"
#include "mon/fil.h"
#include "mon/mon.h"
#include "mon/sys.h"
#include "pico/stdlib.h"
#include <stdio.h>

static size_t capture_length;
static void (*capture_callback)(void);
uint32_t capture_timeout_ms;
static absolute_time_t timer;

void com_init()
{
    com_reclock();
}

void com_reset()
{
    capture_callback = NULL;
    com_preclock();
}

void com_preclock()
{
    // flush every buffer
    while (getchar_timeout_us(0) >= 0)
        tight_loop_contents();
    while (!(uart_get_hw(COM_UART)->fr & UART_UARTFR_TXFE_BITS))
        tight_loop_contents();
}

void com_reclock()
{
    stdio_uart_init_full(COM_UART, RIA_UART_BAUD_RATE,
                         RIA_UART_TX_PIN, RIA_UART_RX_PIN);
}

size_t com_write(char *ptr, size_t count)
{
    size_t bw = 0;
    for (; count && uart_is_writable(COM_UART); --count, bw++)
    {
        uint8_t ch = *(uint8_t *)ptr++;
        if (ch == '\n')
        {
            uart_putc_raw(COM_UART, '\r');
            uart_putc_raw(COM_UART, ch);
        }
        else
            uart_get_hw(COM_UART)->dr = ch;
    }
    return bw;
}

void com_task()
{
    // Detect UART breaks.
    static uint32_t break_detect = 0;
    uint32_t current_break = uart_get_hw(COM_UART)->rsr & UART_UARTRSR_BE_BITS;
    if (current_break)
        hw_clear_bits(&uart_get_hw(COM_UART)->rsr, UART_UARTRSR_BITS);
    else if (break_detect)
        main_break();
    break_detect = current_break;

    // Allow UART RX FIFO to fill during RIA actions.
    // At all other times the FIFO must be emptied to detect breaks.
    if (!ria_is_running())
    {
        int ch = getchar_timeout_us(0);
        while (ch != PICO_ERROR_TIMEOUT)
        {
            if (capture_callback)
            {
                absolute_time_t now = get_absolute_time();
                mbuf[mbuf_len] = ch;
                if (++mbuf_len == capture_length || absolute_time_diff_us(now, timer) < 0)
                {
                    void (*cc)(void) = capture_callback;
                    capture_callback = NULL;
                    cc();
                }
                timer = delayed_by_ms(now, capture_timeout_ms);
            }
            else if (cpu_is_running())
                ria_com_rx(ch);
            else
                mon_com_rx(ch);
            if (ria_is_running())
                break;
            ch = getchar_timeout_us(0);
        }
    }
}

void com_capture_mbuf(void (*callback)(void), size_t length, uint32_t timeout_ms)
{
    mbuf_len = 0;
    capture_length = length;
    capture_timeout_ms = timeout_ms;
    timer = delayed_by_ms(get_absolute_time(), capture_timeout_ms);
    capture_callback = callback;
}
