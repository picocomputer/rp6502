/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga/sys/com.h"
#include "core/sys/com_term.h"
#include "core/sys/ring.h"
#include "vga/usb/cdc.h"
#include <tusb.h>
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>

static RING(char, COM_IN_BUF_SIZE) com_in;

static char com_in_reply_buf[COM_IN_BUF_SIZE];
static size_t com_in_reply_len;

static RING(char, COM_OUT_BUF_SIZE) com_out;

static bool com_term_reply_suppressed;

void com_suppress_term_reply(bool suppress)
{
    com_term_reply_suppressed = suppress;
}

size_t com_in_free(void)
{
    return RING_AVAIL(&com_in);
}

bool com_in_empty(void)
{
    return !RING_LEN(&com_in);
}

void com_in_write(char ch)
{
    RING_PUT(&com_in, ch);
}

void com_in_write_reply(const char *s, size_t n)
{
    if (com_term_reply_suppressed || cdc_is_ready())
        return;
    if (n > sizeof(com_in_reply_buf) - com_in_reply_len)
        return;
    for (size_t i = 0; i < n; i++)
        com_in_reply_buf[com_in_reply_len++] = s[i];
}

bool com_out_empty(void)
{
    return !RING_LEN(&com_out);
}

bool com_out_full(void)
{
    return !RING_AVAIL(&com_out);
}

char com_out_peek(void)
{
    return *RING_AT(&com_out, 0);
}

char com_out_read(void)
{
    char ch = 0;
    RING_GET(&com_out, &ch);
    return ch;
}

static void com_out_chars(const char *buf, int length)
{
    for (;;)
    {
        const uint32_t n = RING_IN(&com_out, buf, (uint32_t)length);
        buf += n;
        length -= (int)n;
        if (!length)
            break;
        cdc_task();
        tud_task();
    }
}

void com_init(void)
{
    gpio_pull_up(COM_UART_TX_PIN);
    gpio_pull_up(COM_UART_RX_PIN);
    gpio_set_function(COM_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(COM_UART_RX_PIN, GPIO_FUNC_UART);
    uart_init(COM_UART_INTERFACE, COM_UART_BAUDRATE);

    static stdio_driver_t stdio_driver = {
        .out_chars = com_out_chars,
        .crlf_enabled = true,
    };
    stdio_set_driver_enabled(&stdio_driver, true);
}

void com_set_term_out(void (*out_chars)(const char *buf, int len))
{
    static stdio_driver_t term_driver = {.crlf_enabled = true};
    term_driver.out_chars = out_chars;
    stdio_set_driver_enabled(&term_driver, out_chars != NULL);
}

/* uart_init in com_post_reclock resets the UART, which empties both FIFOs
 * and clears a break in progress. */
static bool com_reclock_break;

void com_pre_reclock(void)
{
    while (uart_is_readable(COM_UART_INTERFACE))
        putchar_raw(uart_getc(COM_UART_INTERFACE));
    com_reclock_break = uart_get_hw(COM_UART_INTERFACE)->lcr_h & UART_UARTLCR_H_BRK_BITS;
    if (com_reclock_break)
        return;
    uart_tx_wait_blocking(COM_UART_INTERFACE);
}

void com_post_reclock(void)
{
    uart_init(COM_UART_INTERFACE, COM_UART_BAUDRATE);
    if (com_reclock_break)
        uart_set_break(COM_UART_INTERFACE, true);
}

void com_set_uart_break(bool en)
{
    uart_set_break(COM_UART_INTERFACE, en);
}

void com_task(void)
{
    if (com_in_reply_len && !RING_LEN(&com_in))
    {
        RING_IN(&com_in, com_in_reply_buf, (uint32_t)com_in_reply_len);
        com_in_reply_len = 0;
    }

    // IN is sunk here to UART
    char ch;
    while (uart_is_writable(COM_UART_INTERFACE) && RING_GET(&com_in, &ch))
        uart_get_hw(COM_UART_INTERFACE)->dr = ch;

    // OUT is sourced here from STD UART
    while (uart_is_readable(COM_UART_INTERFACE))
        putchar_raw(uart_getc(COM_UART_INTERFACE));
}
