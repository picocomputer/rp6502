/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/main.h"
#include "ria.h"
#include "ria_action.h"
#include "ria_uart.h"
#include "pico/stdlib.h"

volatile int ria_uart_rx_char;
static size_t ria_in_start;
static size_t ria_in_end;
static uint8_t ria_in_buf[32];
#define RIA_IN_BUF(pos) ria_in_buf[(pos) & 0x1F]

void ria_uart_init()
{
    stdio_uart_init_full(RIA_UART, RIA_UART_BAUD_RATE,
                         RIA_UART_TX_PIN, RIA_UART_RX_PIN);
    ria_uart_reset();
}

void ria_uart_reset()
{
    ria_uart_rx_char = -1;
    ria_in_start = ria_in_end = 0;
    ria_uart_flush();
}

void ria_uart_flush()
{
    while (getchar_timeout_us(0) >= 0)
        tight_loop_contents();
    while (!(uart_get_hw(RIA_UART)->fr & UART_UARTFR_TXFE_BITS))
        tight_loop_contents();
}

// Do a caps conversion with current setting
static uint8_t ria_uart_caps_ch(uint8_t ch)
{
    switch (ria_get_caps())
    {
    case 1:
        if (ch >= 'A' && ch <= 'Z')
        {
            ch += 32;
            break;
        }
        // fall through
    case 2:
        if (ch >= 'a' && ch <= 'z')
            ch -= 32;
    }
    return ch;
}

void ria_uart_task()
{
    // Reset 6502 when UART break signal received
    static uint32_t break_detect = 0;
    uint32_t current_break = uart_get_hw(RIA_UART)->rsr & UART_UARTRSR_BE_BITS;
    if (current_break)
        hw_clear_bits(&uart_get_hw(RIA_UART)->rsr, UART_UARTRSR_BITS);
    else if (break_detect)
        main_break();
    break_detect = current_break;

    // We need to keep UART FIFO empty or breaks won't come in.
    // This maintains a buffer and feeds ria_uart_rx_char to the action loop.
    if (!ria_action_in_progress() && ria_is_active())
    {
        int ch = getchar_timeout_us(0);
        if (ch >= 0 && &RIA_IN_BUF(ria_in_end + 1) != &RIA_IN_BUF(ria_in_start))
            RIA_IN_BUF(++ria_in_end) = ch;
        if (ria_uart_rx_char < 0 && &RIA_IN_BUF(ria_in_end) != &RIA_IN_BUF(ria_in_start))
            ria_uart_rx_char = ria_uart_caps_ch(RIA_IN_BUF(++ria_in_start));
    }
}
