/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/proc.h"
#include "core/com/com.h"
#include "core/sys/ria.h"
#include "core/aud/bel.h"
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "ria/sys/vga.h"
#include "core/sys/ring.h"
#include "ria/com/manifold.h"
#include "ria/com/com_telnet.h"
#include "machine.h"
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>
#include <stdio.h>

// The producers of this ring, the stdio driver and manifold_putchar_raw, and
// its consumer, manifold_tx_fanout, all run on the core 0 main loop.
static RING(uint8_t, 32) manifold_tx_core0;

/* manifold_stdio_driver sets crlf_enabled, so a newline can be written to the
 * ring as CR LF, and manifold_cts asks for room for both bytes. */
bool manifold_cts(void)
{
    return RING_AVAIL(&manifold_tx_core0) >= 2;
}

void manifold_putchar_raw(char c)
{
    RING_PUT(&manifold_tx_core0, (uint8_t)c);
}

/* stderr goes out as stdout does. */
size_t manifold_write(int fd, const char *buf, size_t n)
{
    (void)fd;
    size_t i = 0;
    for (; i < n && manifold_cts(); i++)
        manifold_putchar(buf[i]);
    return i;
}

static RING(uint8_t, 32) manifold_uart_tx;
static RING(uint8_t, 32) manifold_uart_rx;

void HOST_IN_RAM_FUNC manifold_uart_drain_rx(void)
{
    while (uart_is_readable(MANIFOLD_UART))
    {
        uint8_t c = (uint8_t)uart_get_hw(MANIFOLD_UART)->dr;
        if (c == 0x03)
            ria_trigger_sigint();
        RING_PUT(&manifold_uart_rx, c);
    }
}

/* manifold_task drains the UART's hardware FIFO into the ring once per pass. */
size_t HOST_IN_RAM_FUNC manifold_uart_read(char *buf, size_t length)
{
    return RING_OUT(&manifold_uart_rx, buf, (uint32_t)length);
}

int HOST_IN_RAM_FUNC manifold_uart_peek(void)
{
    return RING_LEN(&manifold_uart_rx) ? *RING_AT(&manifold_uart_rx, 0) : -1;
}

void HOST_IN_FLASH_FUNC manifold_uart_clear(void)
{
    manifold_uart_drain_rx();
    RING_SKIP(&manifold_uart_rx, RING_LEN(&manifold_uart_rx));
}

static bool HOST_IN_RAM_FUNC manifold_uart_tx_writable(void)
{
    return RING_AVAIL(&manifold_uart_tx) != 0;
}

static void manifold_uart_tx_write(char ch)
{
    RING_PUT(&manifold_uart_tx, (uint8_t)ch);
}

static void HOST_IN_RAM_FUNC manifold_uart_drain_tx(void)
{
    bool vga = vga_connected();
    while (RING_LEN(&manifold_uart_tx))
    {
        uint32_t fr = uart_get_hw(MANIFOLD_UART)->fr;
        if (vga)
        {
            if (!(fr & UART_UARTFR_TXFE_BITS) || !pix_ready())
                break;
        }
        else if (fr & UART_UARTFR_TXFF_BITS)
            break;
        uint8_t ch = *RING_AT(&manifold_uart_tx, 0);
        RING_SKIP(&manifold_uart_tx, 1);
        uart_putc_raw(MANIFOLD_UART, (char)ch);
        if (vga)
            pix_send(PIX_DEVICE_VGA, 0xF, 0x03, ch);
        if (ch == '\a' && com_get_bel())
            bel_add(&bel_teletype);
    }
}

static void manifold_uart_flush(void)
{
    while (RING_LEN(&manifold_uart_tx))
        manifold_uart_drain_tx();
    while (uart_get_hw(MANIFOLD_UART)->fr & UART_UARTFR_BUSY_BITS)
        tight_loop_contents();
}

/* Bytes go out in runs: a ring keeps the wire until it is empty, so the RIA's
 * own output and the 6502's TX register never interleave inside an escape
 * sequence. The 6502's run gives way when core 0's ring is full, because a
 * printf waits on that ring, while the 6502 waits on READY without harm. */
static bool manifold_tx_from_ria;

static bool HOST_IN_RAM_FUNC manifold_tx_next(uint8_t *ch)
{
    return manifold_tx_from_ria ? ria_uart_tx_dequeue(ch) : RING_GET(&manifold_tx_core0, ch);
}

static void HOST_IN_RAM_FUNC manifold_tx_fanout(void)
{
    while (manifold_uart_tx_writable() && com_telnet_tx_writable())
    {
        if (manifold_tx_from_ria && !RING_AVAIL(&manifold_tx_core0))
            manifold_tx_from_ria = false;
        uint8_t ch;
        if (!manifold_tx_next(&ch))
        {
            manifold_tx_from_ria = !manifold_tx_from_ria;
            if (!manifold_tx_next(&ch))
                break;
        }
        com_line_track((const char *)&ch, 1);
        manifold_uart_tx_write((char)ch);
        com_telnet_tx_write((char)ch);
    }
}

static void manifold_stdio_pump(void)
{
    manifold_tx_fanout();
    manifold_uart_drain_tx();
    manifold_uart_drain_rx();
    com_telnet_pump();
}

static void manifold_stdio_out_chars(const char *buf, int len)
{
    while (len--)
    {
        while (!RING_AVAIL(&manifold_tx_core0))
            manifold_stdio_pump();
        manifold_putchar_raw(*buf++);
    }
}

static void manifold_stdio_out_flush(void)
{
    while (RING_LEN(&manifold_tx_core0))
        manifold_stdio_pump();
    manifold_uart_flush();
}

static int manifold_stdio_in_chars(char *buf, int length)
{
    size_t count = com_stdin_read(buf, (size_t)length);
    return count ? (int)count : PICO_ERROR_NO_DATA;
}

static stdio_driver_t manifold_stdio_driver = {
    .out_chars = manifold_stdio_out_chars,
    .out_flush = manifold_stdio_out_flush,
    .in_chars = manifold_stdio_in_chars,
    .crlf_enabled = true,
};

void HOST_IN_FLASH_FUNC manifold_init(void)
{
    gpio_pull_up(MANIFOLD_UART_TX_PIN);
    gpio_pull_up(MANIFOLD_UART_RX_PIN);
    gpio_set_function(MANIFOLD_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(MANIFOLD_UART_RX_PIN, GPIO_FUNC_UART);
    stdio_set_driver_enabled(&manifold_stdio_driver, true);
    uart_init(MANIFOLD_UART, MANIFOLD_UART_BAUD_RATE);
    // Wait for the UART to settle after VGA startup then purge everything.
    busy_wait_ms(30);
    manifold_uart_clear();
    hw_clear_bits(&uart_get_hw(MANIFOLD_UART)->rsr, UART_UARTRSR_BITS);
}

void manifold_flush(void)
{
    while (!ria_uart_tx_empty() || RING_LEN(&manifold_tx_core0))
        manifold_stdio_pump();
    manifold_uart_flush();
}

void HOST_IN_RAM_FUNC manifold_task(void)
{
    manifold_uart_drain_tx();

    manifold_tx_fanout();

    manifold_uart_drain_rx();

    if (ria_uart_rx_offer_ready())
        com_rx_stage();

    // Detect UART breaks.
    static uint32_t break_detect = 0;
    uint32_t current_break = uart_get_hw(MANIFOLD_UART)->rsr & UART_UARTRSR_BE_BITS;
    if (current_break)
        hw_clear_bits(&uart_get_hw(MANIFOLD_UART)->rsr, UART_UARTRSR_BITS);
    else if (break_detect)
        proc_break();
    break_detect = current_break;
}

int manifold_putchar(int c)
{
    return putchar(c);
}
