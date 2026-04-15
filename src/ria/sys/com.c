/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "aud/bel.h"
#include "sys/com.h"
#include "sys/pix.h"
#include "sys/vga.h"
#include <pico/stdlib.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_COM)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static bool com_bel_enabled = true;

/* TX buffer — internal
 */

#define COM_TX_BUF_SIZE 32
static volatile size_t com_tx_tail;
static volatile size_t com_tx_head;
static volatile uint8_t com_tx_buf[COM_TX_BUF_SIZE];

static void com_tx_task(void)
{
    if (vga_connected())
    {
        // Use TXFE (empty) to pace VGA PIX sends
        while (com_tx_head != com_tx_tail &&
               uart_get_hw(COM_UART)->fr & UART_UARTFR_TXFE_BITS &&
               pix_ready())
        {
            com_tx_tail = (com_tx_tail + 1) % COM_TX_BUF_SIZE;
            char ch = com_tx_buf[com_tx_tail];
            uart_putc_raw(COM_UART, ch);
            pix_send(PIX_DEVICE_VGA, 0xF, 0x03, ch);
            if (ch == '\a' && com_bel_enabled)
                bel_add(&bel_teletype);
        }
    }
    else
    {
        // Fill UART TX FIFO
        while (com_tx_head != com_tx_tail &&
               !(uart_get_hw(COM_UART)->fr & UART_UARTFR_TXFF_BITS))
        {
            com_tx_tail = (com_tx_tail + 1) % COM_TX_BUF_SIZE;
            char ch = com_tx_buf[com_tx_tail];
            uart_putc_raw(COM_UART, ch);
            if (ch == '\a' && com_bel_enabled)
                bel_add(&bel_teletype);
        }
    }
}

/* TX — for tee
 */

bool com_tx_writable(void)
{
    return (((com_tx_head + 1) % COM_TX_BUF_SIZE) != com_tx_tail);
}

void com_tx_write(char ch)
{
    com_tx_head = (com_tx_head + 1) % COM_TX_BUF_SIZE;
    com_tx_buf[com_tx_head] = ch;
}

void com_pump(void)
{
    com_tx_task();
}

void com_flush(void)
{
    while (com_tx_head != com_tx_tail)
        com_tx_task();
    while (uart_get_hw(COM_UART)->fr & UART_UARTFR_BUSY_BITS)
        tight_loop_contents();
}

/* RX — for tee
 */

int com_rx(char *buf, int length)
{
    int count = 0;
    while (uart_is_readable(COM_UART) && count < length)
        buf[count++] = (uint8_t)uart_get_hw(COM_UART)->dr;
    return count ? count : PICO_ERROR_NO_DATA;
}

/* Main events
 */

void com_init(void)
{
    gpio_set_function(COM_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(COM_UART_RX_PIN, GPIO_FUNC_UART);
    uart_init(COM_UART, COM_UART_BAUD_RATE);
}

void com_task(void)
{
    // Process transmit.
    com_tx_task();

    // Detect UART breaks.
    static uint32_t break_detect = 0;
    uint32_t current_break = uart_get_hw(COM_UART)->rsr & UART_UARTRSR_BE_BITS;
    if (current_break)
        hw_clear_bits(&uart_get_hw(COM_UART)->rsr, UART_UARTRSR_BITS);
    else if (break_detect)
        main_break();
    break_detect = current_break;
}

bool com_get_bel(void)
{
    return com_bel_enabled;
}

void com_set_bel(bool value)
{
    com_bel_enabled = value;
}
