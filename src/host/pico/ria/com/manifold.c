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
#include <string.h>

/* One byte of core 0's output and the COM_TERM_ bits of the terminals it is
 * for. */
typedef struct
{
    uint8_t ch;
    uint8_t to;
} manifold_tx_entry_t;

// The producers of this ring, the stdio driver, manifold_putchar_raw and
// manifold_write_to, and its consumer, manifold_tx_fanout, all run on the
// core 0 main loop.
static RING(manifold_tx_entry_t, 32) manifold_tx_core0;

/* manifold_stdio_driver sets crlf_enabled, so a newline can be written to the
 * ring as CR LF, and manifold_cts asks for room for both bytes. */
bool manifold_cts(void)
{
    return RING_AVAIL(&manifold_tx_core0) >= 2;
}

static void manifold_tx_put(uint8_t to, char c)
{
    RING_PUT(&manifold_tx_core0, ((manifold_tx_entry_t){.ch = (uint8_t)c, .to = to}));
}

void manifold_putchar_raw(char c)
{
    manifold_tx_put(COM_TERM_ALL, c);
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
static RING(uint8_t, MANIFOLD_RING_SIZE) manifold_uart_rx;

/* The console's terminal's answers to queries, held whole until the UART's
 * ring is empty, so one never lands inside a sequence still arriving there.
 * The terminal is fed every byte the VGA's terminal gets, as it is sent. */
static uint8_t manifold_reply_buf[32];
static size_t manifold_reply_len;
static bool manifold_reply_suppressed;
static void (*manifold_term_out)(const char *buf, int len);

static void HOST_IN_RAM_FUNC manifold_reply_promote(void)
{
    if (!manifold_reply_len || RING_LEN(&manifold_uart_rx))
        return;
    RING_IN(&manifold_uart_rx, manifold_reply_buf, (uint32_t)manifold_reply_len);
    manifold_reply_len = 0;
}

/* A reply that does not fit is dropped whole rather than truncated, because
 * half of an escape sequence is a sequence the reader parses as something
 * else. */
void manifold_reply(const char *s, size_t n)
{
    if (manifold_reply_suppressed || n > sizeof manifold_reply_buf ||
        manifold_reply_len > sizeof manifold_reply_buf - n)
        return;
    memcpy(manifold_reply_buf + manifold_reply_len, s, n);
    manifold_reply_len += n;
}

void manifold_suppress_reply(bool suppress)
{
    manifold_reply_suppressed = suppress;
}

bool manifold_replies_suppressed(void)
{
    return manifold_reply_suppressed;
}

void manifold_set_term_out(void (*out_chars)(const char *buf, int len))
{
    manifold_term_out = out_chars;
}

/* One byte on its way to the VGA's terminal, or to the wire with no VGA, to
 * the console's terminal too. */
static void HOST_IN_RAM_FUNC manifold_term_feed(char ch)
{
    if (manifold_term_out)
        manifold_term_out(&ch, 1);
}

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
    manifold_reply_promote();
    return RING_OUT(&manifold_uart_rx, buf, (uint32_t)length);
}

/* A held reply comes after the ring, as manifold_uart_read delivers it. */
int HOST_IN_RAM_FUNC manifold_uart_peek(size_t offset)
{
    manifold_reply_promote();
    size_t len = RING_LEN(&manifold_uart_rx);
    if (offset < len)
        return *RING_AT(&manifold_uart_rx, offset);
    offset -= len;
    return offset < manifold_reply_len ? manifold_reply_buf[offset] : -1;
}

void HOST_IN_FLASH_FUNC manifold_uart_clear(void)
{
    manifold_uart_drain_rx();
    RING_SKIP(&manifold_uart_rx, RING_LEN(&manifold_uart_rx));
    manifold_reply_len = 0;
}

static bool HOST_IN_RAM_FUNC manifold_uart_tx_writable(void)
{
    return RING_AVAIL(&manifold_uart_tx) != 0;
}

static void manifold_uart_tx_write(char ch)
{
    RING_PUT(&manifold_uart_tx, (uint8_t)ch);
}

/* With the VGA connected, the UART's TX pin carries the backchannel, so a
 * byte written to the UART reaches no wire. Each byte sent to the VGA over
 * PIX still goes through the UART, which spaces them one character time
 * apart, so the VGA's small PIX FIFO is never overrun. */
static bool HOST_IN_RAM_FUNC manifold_vga_writable(void)
{
    return (uart_get_hw(MANIFOLD_UART)->fr & UART_UARTFR_TXFE_BITS) && pix_ready();
}

static void HOST_IN_RAM_FUNC manifold_vga_write(uint8_t reg, char ch)
{
    uart_putc_raw(MANIFOLD_UART, ch);
    pix_send(PIX_DEVICE_VGA, 0xF, reg, (uint8_t)ch);
}

static void HOST_IN_RAM_FUNC manifold_uart_drain_tx(void)
{
    bool vga = vga_connected();
    while (RING_LEN(&manifold_uart_tx))
    {
        if (vga ? !manifold_vga_writable()
                : (uart_get_hw(MANIFOLD_UART)->fr & UART_UARTFR_TXFF_BITS))
            break;
        char ch = (char)*RING_AT(&manifold_uart_tx, 0);
        RING_SKIP(&manifold_uart_tx, 1);
        if (vga)
            manifold_vga_write(0x03, ch);
        else
            uart_putc_raw(MANIFOLD_UART, ch);
        manifold_term_feed(ch);
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

static bool HOST_IN_RAM_FUNC manifold_tx_waiting(void)
{
    return manifold_tx_from_ria ? !ria_uart_tx_empty() : RING_LEN(&manifold_tx_core0) != 0;
}

/* A byte for the screen's terminal alone goes over PIX as TERM_TX, never on
 * the UART wire. It waits for the UART's ring to empty, because the bytes in
 * that ring reach the screen over PIX as UART_TX as they leave it. With no
 * VGA only the console's terminal gets it. */
static bool HOST_IN_RAM_FUNC manifold_screen_writable(void)
{
    return !vga_connected() || (!RING_LEN(&manifold_uart_tx) && manifold_vga_writable());
}

static void HOST_IN_RAM_FUNC manifold_screen_write(char ch)
{
    if (vga_connected())
        manifold_vga_write(0x05, ch);
    manifold_term_feed(ch);
}

/* Each entry goes, in order, to the terminals it is for, and waits only for
 * room in their rings. The 6502's output is for every terminal. term.c hears
 * the UART, so a byte for both goes to the UART alone. */
static void HOST_IN_RAM_FUNC manifold_tx_fanout(void)
{
    while (true)
    {
        if (manifold_tx_from_ria && !RING_AVAIL(&manifold_tx_core0))
            manifold_tx_from_ria = false;
        if (!manifold_tx_waiting())
        {
            manifold_tx_from_ria = !manifold_tx_from_ria;
            if (!manifold_tx_waiting())
                break;
        }
        uint8_t to = manifold_tx_from_ria ? COM_TERM_ALL : RING_AT(&manifold_tx_core0, 0)->to;
        if ((to & COM_TERM_UART) ? !manifold_uart_tx_writable()
                                 : (to & COM_TERM_SCREEN) && !manifold_screen_writable())
            break;
        if ((to & COM_TERM_TEL) && !com_telnet_tx_writable())
            break;
        uint8_t ch;
        if (manifold_tx_from_ria)
            ria_uart_tx_dequeue(&ch);
        else
        {
            ch = RING_AT(&manifold_tx_core0, 0)->ch;
            RING_SKIP(&manifold_tx_core0, 1);
        }
        if (to & COM_TERM_UART)
            manifold_uart_tx_write((char)ch);
        else if (to & COM_TERM_SCREEN)
            manifold_screen_write((char)ch);
        if (to & COM_TERM_TEL)
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

void manifold_write_to(uint8_t to, const char *buf, size_t n)
{
    while (n--)
    {
        while (!RING_AVAIL(&manifold_tx_core0))
            manifold_stdio_pump();
        manifold_tx_put(to, *buf++);
    }
}

static void manifold_stdio_out_chars(const char *buf, int len)
{
    manifold_write_to(COM_TERM_ALL, buf, (size_t)len);
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
    /* The Tx pin doubles as the VGA's backchannel. A VGA that stayed up while
     * the RIA reset may still be driving it, so the pin stays an input until
     * vga.c finds out who owns it. */
    gpio_pull_up(MANIFOLD_UART_TX_PIN);
    gpio_pull_up(MANIFOLD_UART_RX_PIN);
    gpio_init(MANIFOLD_UART_TX_PIN);
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
