/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/sys.h"
#include "core/sys/ria.h"
#include "core/str/oem.h"
#include "core/aud/bel.h"
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "ria/sys/vga.h"
#include "core/str/str.h"
#include "core/sys/ring.h"
#include "ria/sys/com.h"
#include "ria/sys/com_telnet.h"
#include <stdlib.h>
#include <string.h>
#include <pico/stdlib.h>
#include <pico/printf.h>
#include <pico/stdio/driver.h>
#include <stdio.h>

// The producers of this ring, stdio and std_tty_write, and its consumer,
// com_tx_fanout, all run on the core 0 main loop.
static RING(uint8_t, 32) com_tx_core0;

bool com_putchar_ready(void)
{
    return RING_AVAIL(&com_tx_core0) >= 2;
}

bool com_writable(void)
{
    return RING_AVAIL(&com_tx_core0) != 0;
}

void com_write(char ch)
{
    RING_PUT(&com_tx_core0, (uint8_t)ch);
}

size_t com_stdout_write(const char *buf, size_t count)
{
    size_t i = 0;
    for (; i < count && com_putchar_ready(); i++)
        com_putchar(buf[i]);
    return i;
}

size_t com_stderr_write(const char *buf, size_t count)
{
    return com_stdout_write(buf, count);
}

static RING(uint8_t, 32) com_uart_tx;
static RING(uint8_t, 32) com_uart_rx;

static com_source_t com_rx_char_src;

size_t com_rx_reclaim(char *buf, size_t length, com_source_t src)
{
    uint8_t ch;
    if (length && com_rx_char_src == src && ria_uart_rx_reclaim(&ch))
    {
        buf[0] = (char)ch;
        return 1;
    }
    return 0;
}

int com_rx_peek(com_source_t src)
{
    return com_rx_char_src == src ? ria_uart_rx_peek() : -1;
}

static bool com_bel_enabled = true;


static void com_uart_drain_rx(void)
{
    while (uart_is_readable(COM_UART))
    {
        uint8_t c = (uint8_t)uart_get_hw(COM_UART)->dr;
        if (c == 0x03)
            ria_trigger_sigint();
        RING_PUT(&com_uart_rx, c);
    }
}

size_t com_uart_read(char *buf, size_t length)
{
    // The UART's hardware FIFO is drained here as well as in com_task, because
    // input is also read by loops that do not return to com_task between
    // reads, such as the ones in com_init, vga_connect and the monitor.
    com_uart_drain_rx();
    return RING_OUT(&com_uart_rx, buf, (uint32_t)length);
}

int com_uart_peek(void)
{
    com_uart_drain_rx();
    return RING_LEN(&com_uart_rx) ? *RING_AT(&com_uart_rx, 0) : -1;
}

void com_uart_clear(void)
{
    com_uart_drain_rx();
    RING_SKIP(&com_uart_rx, RING_LEN(&com_uart_rx));
}

static bool com_uart_tx_writable(void)
{
    return RING_AVAIL(&com_uart_tx) != 0;
}

static void com_uart_tx_write(char ch)
{
    RING_PUT(&com_uart_tx, (uint8_t)ch);
}

static void com_uart_drain_tx(void)
{
    bool vga = vga_connected();
    uint8_t ch;
    while (RING_LEN(&com_uart_tx))
    {
        uint32_t fr = uart_get_hw(COM_UART)->fr;
        if (vga)
        {
            if (!(fr & UART_UARTFR_TXFE_BITS) || !pix_ready())
                break;
        }
        else if (fr & UART_UARTFR_TXFF_BITS)
            break;
        RING_GET(&com_uart_tx, &ch);
        uart_putc_raw(COM_UART, (char)ch);
        if (vga)
            pix_send(PIX_DEVICE_VGA, 0xF, 0x03, ch);
        if (ch == '\a' && com_bel_enabled)
            bel_add(&bel_teletype);
    }
}

static void com_uart_flush(void)
{
    while (RING_LEN(&com_uart_tx))
        com_uart_drain_tx();
    while (uart_get_hw(COM_UART)->fr & UART_UARTFR_BUSY_BITS)
        tight_loop_contents();
}

static void com_tx_fanout(void)
{
    while (com_uart_tx_writable() && com_telnet_tx_writable())
    {
        bool work = false;
        uint8_t ch;
        if (RING_GET(&com_tx_core0, &ch))
        {
            com_uart_tx_write((char)ch);
            com_telnet_tx_write((char)ch);
            work = true;
            if (!com_uart_tx_writable() || !com_telnet_tx_writable())
                break;
        }
        uint8_t ch1;
        if (ria_uart_tx_dequeue(&ch1))
        {
            com_uart_tx_write((char)ch1);
            com_telnet_tx_write((char)ch1);
            work = true;
        }
        if (!work)
            break;
    }
}

static void com_stdio_pump(void)
{
    com_tx_fanout();
    com_uart_drain_tx();
    com_uart_drain_rx();
    com_telnet_pump();
}

static void com_stdio_out_chars(const char *buf, int len)
{
    while (len--)
    {
        while (!com_writable())
            com_stdio_pump();
        com_write(*buf++);
    }
}

static void com_stdio_out_flush(void)
{
    while (RING_LEN(&com_tx_core0))
        com_stdio_pump();
    com_uart_flush();
}

static int com_stdio_in_chars(char *buf, int length)
{
    size_t count = com_stdin_read(buf, (size_t)length);
    return count ? (int)count : PICO_ERROR_NO_DATA;
}

static stdio_driver_t com_stdio_driver = {
    .out_chars = com_stdio_out_chars,
    .out_flush = com_stdio_out_flush,
    .in_chars = com_stdio_in_chars,
    .crlf_enabled = true,
};

void __in_flash("com_init") com_init(void)
{
    gpio_pull_up(COM_UART_TX_PIN);
    gpio_pull_up(COM_UART_RX_PIN);
    gpio_set_function(COM_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(COM_UART_RX_PIN, GPIO_FUNC_UART);
    stdio_set_driver_enabled(&com_stdio_driver, true);
    uart_init(COM_UART, COM_UART_BAUD_RATE);
    // Wait for the UART to settle after VGA startup then purge everything.
    busy_wait_ms(30);
    while (stdio_getchar_timeout_us(0) != PICO_ERROR_TIMEOUT)
        tight_loop_contents();
    hw_clear_bits(&uart_get_hw(COM_UART)->rsr, UART_UARTRSR_BITS);
}

void com_run(void)
{
    com_bel_enabled = true;
}

void com_stop(void)
{
    while (!ria_uart_tx_empty())
        com_stdio_pump();
    printf(STR_TERM_SOFT_RESET);
    while (!com_putchar_ready())
        com_stdio_pump();
}

static void com_ensure_newline(void)
{
    const uint32_t n = RING_LEN(&com_tx_core0);
    if (n < 2 ||
        *RING_AT(&com_tx_core0, n - 1) != '\n' ||
        *RING_AT(&com_tx_core0, n - 2) != '\r')
        putchar('\n');
}

void com_break(void)
{
    com_ensure_newline();
    com_rx_clear();
}

void com_task(void)
{
    com_uart_drain_tx();

    com_tx_fanout();

    com_uart_drain_rx();

    if (ria_uart_rx_offer_ready())
    {
        com_source_t src = COM_SOURCE_ANY;
        int ch = com_getchar(&src);
        if (ch >= 0)
        {
            com_rx_char_src = src;
            ria_uart_rx_offer((uint8_t)ch);
        }
    }

    // Detect UART breaks.
    static uint32_t break_detect = 0;
    uint32_t current_break = uart_get_hw(COM_UART)->rsr & UART_UARTRSR_BE_BITS;
    if (current_break)
        hw_clear_bits(&uart_get_hw(COM_UART)->rsr, UART_UARTRSR_BITS);
    else if (break_detect)
        sys_break();
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

int com_putchar(int c)
{
    return putchar(c);
}

int com_printf(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = vprintf(fmt, va);
    va_end(va);
    return n;
}

#define COM_PRINTF_UTF8_SIZE 128

int com_printf_utf8(const char *utf8_fmt, ...)
{
    char buf[COM_PRINTF_UTF8_SIZE];
    va_list va;
    va_start(va, utf8_fmt);
    oem_vsnprintf(buf, sizeof(buf), utf8_fmt, va);
    va_end(va);
    int n = 0;
    for (const char *p = buf; *p; p++, n++)
        com_putchar(*p);
    return n;
}
