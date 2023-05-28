/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cfg.h"
#include "com.h"
#include "cpu.h"
#include "main.h"
#include "ria.h"
#include "pico/stdlib.h"

void com_init()
{
    com_reclock();
}

void com_reset()
{
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
    // Detect UART break
    static uint32_t break_detect = 0;
    uint32_t current_break = uart_get_hw(COM_UART)->rsr & UART_UARTRSR_BE_BITS;
    if (current_break)
        hw_clear_bits(&uart_get_hw(COM_UART)->rsr, UART_UARTRSR_BITS);
    else if (break_detect)
        main_break();
    break_detect = current_break;

    // We need to keep UART FIFO empty or breaks won't come in.
    // Receivers will discard overflows.
    if (cpu_is_running())
    {
        int ch = getchar_timeout_us(0);
        if (ch >= 0)
            ria_com_rx(ch);
    }
}
