/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sys/com.h"
#include "usb/cdc.h"
#include <tusb.h>
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>

static size_t com_in_head;
static size_t com_in_tail;
static char com_in_buf[COM_IN_BUF_SIZE];

// Pending term reply, drained into com_in by com_task at a clean
// stream boundary so we never splice into an in-flight CDC sequence.
static char com_in_reply_buf[COM_IN_BUF_SIZE - 1];
static size_t com_in_reply_len;

static size_t com_out_head;
static size_t com_out_tail;
static char com_out_buf[COM_OUT_BUF_SIZE];

static bool com_term_reply_suppressed;

void com_suppress_term_reply(bool suppress)
{
    com_term_reply_suppressed = suppress;
}

size_t com_in_free(void)
{
    return (com_in_tail + COM_IN_BUF_SIZE - com_in_head - 1) % COM_IN_BUF_SIZE;
}

bool com_in_empty(void)
{
    return com_in_head == com_in_tail;
}

void com_in_write(char ch)
{
    com_in_head = (com_in_head + 1) % COM_IN_BUF_SIZE;
    com_in_buf[com_in_head] = ch;
}

// Queue a term-sourced reply for delivery to RIA via com_in.
// Held in a pending slot so com_task can promote the whole reply
// atomically when com_in is empty, avoiding splicing into an
// in-flight CDC byte stream. If a USB terminal is connected it
// will answer the host's queries itself, so we defer to it.
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
    return com_out_head == com_out_tail;
}

bool com_out_full(void)
{
    return (com_out_head + 1) % COM_OUT_BUF_SIZE == com_out_tail;
}

char com_out_peek(void)
{
    return com_out_buf[(com_out_tail + 1) % COM_OUT_BUF_SIZE];
}

char com_out_read(void)
{
    com_out_tail = (com_out_tail + 1) % COM_OUT_BUF_SIZE;
    return com_out_buf[com_out_tail];
}

static void com_out_chars(const char *buf, int length)
{
    while (length)
    {
        size_t next = (com_out_head + 1) % COM_OUT_BUF_SIZE;
        if (next == com_out_tail)
        {
            cdc_task();
            tud_task();
            continue;
        }
        com_out_head = next;
        com_out_buf[com_out_head] = *buf++;
        length--;
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

void com_pre_reclock(void)
{
    // LCR_H.BRK drives TXD low indefinitely, keeping BUSY set forever.
    if (uart_get_hw(COM_UART_INTERFACE)->lcr_h & UART_UARTLCR_H_BRK_BITS)
        return;
    uart_tx_wait_blocking(COM_UART_INTERFACE);
}

void com_post_reclock(void)
{
    uart_init(COM_UART_INTERFACE, COM_UART_BAUDRATE);
}

void com_set_uart_break(bool en)
{
    uart_set_break(COM_UART_INTERFACE, en);
}

void com_task(void)
{
    // Promote any pending term reply at a clean boundary.
    if (com_in_reply_len && com_in_empty())
    {
        for (size_t i = 0; i < com_in_reply_len; i++)
            com_in_write(com_in_reply_buf[i]);
        com_in_reply_len = 0;
    }

    // IN is sunk here to UART
    while (!com_in_empty() && uart_is_writable(COM_UART_INTERFACE))
    {
        com_in_tail = (com_in_tail + 1) % COM_IN_BUF_SIZE;
        uart_get_hw(COM_UART_INTERFACE)->dr = com_in_buf[com_in_tail];
    }

    // OUT is sourced here from STD UART
    while (uart_is_readable(COM_UART_INTERFACE))
        putchar_raw(uart_getc(COM_UART_INTERFACE));
}
