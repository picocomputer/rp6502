/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sys/com.h"
#include "usb/cdc.h"
#include <tusb.h>
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

size_t com_in_head;
size_t com_in_tail;
char com_in_buf[COM_IN_BUF_SIZE];

size_t com_out_head;
size_t com_out_tail;
char com_out_buf[COM_OUT_BUF_SIZE];

static bool com_term_reply_suppressed;

void com_suppress_term_reply(bool suppress)
{
    com_term_reply_suppressed = suppress;
}

static_assert((COM_IN_BUF_SIZE & (COM_IN_BUF_SIZE - 1)) == 0,
              "COM_IN_BUF_SIZE must be a power of 2");
static_assert((COM_OUT_BUF_SIZE & (COM_OUT_BUF_SIZE - 1)) == 0,
              "COM_OUT_BUF_SIZE must be a power of 2");

size_t com_in_free(void)
{
    return ((com_in_tail + 0) - (com_in_head + 1)) & (COM_IN_BUF_SIZE - 1);
}

bool com_in_empty(void)
{
    return com_in_head == com_in_tail;
}

// If USB terminal connected, let it respond instead of us.
// Also requires the input buffer to be empty so we can install the
// reply at offset 0 by setting head/tail directly.
static void com_in_write_reply(const char *s, size_t n)
{
    if (com_term_reply_suppressed || cdc_is_open() ||
        !com_in_empty() || n >= COM_IN_BUF_SIZE)
        return;
    memcpy(com_in_buf, s, n);
    com_in_tail = COM_IN_BUF_SIZE - 1;
    com_in_head = n - 1;
}

// Reports the cursor position
void com_in_write_ansi_CPR(int row, int col)
{
    char buf[COM_IN_BUF_SIZE];
    int n = snprintf(buf, sizeof(buf), "\33[%u;%uR", row, col);
    if (n < 0 || n >= (int)sizeof(buf))
        return;
    com_in_write_reply(buf, n);
}

// Primary device attributes: identify as VT102 (ANSI/ECMA-48)
void com_in_write_ansi_DA(void)
{
    static const char da[] = "\33[?6c";
    com_in_write_reply(da, sizeof(da) - 1);
}

// DSR status: terminal ok
void com_in_write_ansi_DSR_ok(void)
{
    static const char ok[] = "\33[0n";
    com_in_write_reply(ok, sizeof(ok) - 1);
}

void com_in_write(char ch)
{
    com_in_head = (com_in_head + 1) % COM_IN_BUF_SIZE;
    com_in_buf[com_in_head] = ch;
}

bool com_out_empty(void)
{
    return com_out_head == com_out_tail;
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
        while (length && ((com_out_head + 1) % COM_OUT_BUF_SIZE) != com_out_tail)
        {
            com_out_head = (com_out_head + 1) % COM_OUT_BUF_SIZE;
            com_out_buf[com_out_head] = *buf++;
            length--;
        }
        if (((com_out_head + 1) % COM_OUT_BUF_SIZE) == com_out_tail)
        {
            cdc_task();
            tud_task();
        }
    }
}

void com_init(void)
{
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
