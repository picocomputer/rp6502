/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga/com/manifold.h"
#include "core/sys/ring.h"
#include "vga/usb/cdc.h"
#include <tusb.h>
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>

static RING(char, MANIFOLD_IN_BUF_SIZE) manifold_in;

static char manifold_in_reply_buf[MANIFOLD_IN_BUF_SIZE];
static size_t manifold_in_reply_len;

static RING(char, MANIFOLD_OUT_BUF_SIZE) manifold_out;

static bool manifold_reply_suppressed;

void manifold_suppress_reply(bool suppress)
{
    manifold_reply_suppressed = suppress;
}

size_t manifold_in_free(void)
{
    return RING_AVAIL(&manifold_in);
}

bool manifold_in_empty(void)
{
    return !RING_LEN(&manifold_in);
}

void manifold_in_write(char ch)
{
    RING_PUT(&manifold_in, ch);
}

void manifold_reply(const char *s, size_t n)
{
    if (manifold_reply_suppressed || cdc_is_ready())
        return;
    if (n > sizeof(manifold_in_reply_buf) - manifold_in_reply_len)
        return;
    for (size_t i = 0; i < n; i++)
        manifold_in_reply_buf[manifold_in_reply_len++] = s[i];
}

bool manifold_out_empty(void)
{
    return !RING_LEN(&manifold_out);
}

bool manifold_out_full(void)
{
    return !RING_AVAIL(&manifold_out);
}

char manifold_out_peek(void)
{
    return *RING_AT(&manifold_out, 0);
}

char manifold_out_read(void)
{
    char ch = 0;
    RING_GET(&manifold_out, &ch);
    return ch;
}

static void manifold_out_chars(const char *buf, int length)
{
    for (;;)
    {
        const uint32_t n = RING_IN(&manifold_out, buf, (uint32_t)length);
        buf += n;
        length -= (int)n;
        if (!length)
            break;
        cdc_task();
        tud_task();
    }
}

void manifold_init(void)
{
    gpio_pull_up(MANIFOLD_UART_TX_PIN);
    gpio_pull_up(MANIFOLD_UART_RX_PIN);
    gpio_set_function(MANIFOLD_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(MANIFOLD_UART_RX_PIN, GPIO_FUNC_UART);
    uart_init(MANIFOLD_UART_INTERFACE, MANIFOLD_UART_BAUDRATE);

    static stdio_driver_t stdio_driver = {
        .out_chars = manifold_out_chars,
        .crlf_enabled = true,
    };
    stdio_set_driver_enabled(&stdio_driver, true);
}

void manifold_set_term_out(void (*out_chars)(const char *buf, int len))
{
    static stdio_driver_t term_driver = {.crlf_enabled = true};
    term_driver.out_chars = out_chars;
    stdio_set_driver_enabled(&term_driver, out_chars != NULL);
}

/* uart_init in manifold_post_reclock resets the UART, which empties both FIFOs
 * and clears a break in progress. */
static bool manifold_reclock_break;

void manifold_pre_reclock(void)
{
    while (uart_is_readable(MANIFOLD_UART_INTERFACE))
        putchar_raw(uart_getc(MANIFOLD_UART_INTERFACE));
    manifold_reclock_break = uart_get_hw(MANIFOLD_UART_INTERFACE)->lcr_h & UART_UARTLCR_H_BRK_BITS;
    if (manifold_reclock_break)
        return;
    uart_tx_wait_blocking(MANIFOLD_UART_INTERFACE);
}

void manifold_post_reclock(void)
{
    uart_init(MANIFOLD_UART_INTERFACE, MANIFOLD_UART_BAUDRATE);
    if (manifold_reclock_break)
        uart_set_break(MANIFOLD_UART_INTERFACE, true);
}

void manifold_set_uart_break(bool en)
{
    uart_set_break(MANIFOLD_UART_INTERFACE, en);
}

void manifold_task(void)
{
    if (manifold_in_reply_len && !RING_LEN(&manifold_in))
    {
        RING_IN(&manifold_in, manifold_in_reply_buf, (uint32_t)manifold_in_reply_len);
        manifold_in_reply_len = 0;
    }

    // IN is sunk here to UART
    char ch;
    while (uart_is_writable(MANIFOLD_UART_INTERFACE) && RING_GET(&manifold_in, &ch))
        uart_get_hw(MANIFOLD_UART_INTERFACE)->dr = ch;

    // OUT is sourced here from STD UART
    while (uart_is_readable(MANIFOLD_UART_INTERFACE))
        putchar_raw(uart_getc(MANIFOLD_UART_INTERFACE));
}
