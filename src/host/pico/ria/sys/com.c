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
#include "ria/sys/com.h"
#include "ria/sys/com_telnet.h"
#include <stdlib.h>
#include <string.h>
#include <pico/stdlib.h>
#include <pico/printf.h>
#include <pico/stdio/driver.h>
#include <stdio.h>

/* Two TX producers feed com_tx_fanout: stdio / std_tty_write on core 0
 * write to com_tx_core0_buf; act_loop on core 1 (6502 writes to 0xFFE1)
 * fills the ria-owned TX ring, drained here via ria_uart_tx_dequeue().
 * The cross-core rings live in ria.c; everything in this file is core-0
 * main-loop only.
 */

// The core-0-only TX ring. Producers (stdio, std_tty_write) and consumer
// (com_tx_fanout) all run on the core-0 main loop, so the SPSC protocol is
// serialized naturally; no lock, no __dmb() needed.
#define COM_TX_CORE0_BUF_SIZE 32
static uint8_t com_tx_core0_buf[COM_TX_CORE0_BUF_SIZE];
static size_t com_tx_core0_head;
static size_t com_tx_core0_tail;

bool com_putchar_ready(void)
{
    return (
        (((com_tx_core0_head + 1) % COM_TX_CORE0_BUF_SIZE) != com_tx_core0_tail) &&
        (((com_tx_core0_head + 2) % COM_TX_CORE0_BUF_SIZE) != com_tx_core0_tail));
}

bool com_writable(void)
{
    return (((com_tx_core0_head + 1) % COM_TX_CORE0_BUF_SIZE) != com_tx_core0_tail);
}

void com_write(char ch)
{
    size_t next = (com_tx_core0_head + 1) % COM_TX_CORE0_BUF_SIZE;
    com_tx_core0_buf[next] = (uint8_t)ch;
    com_tx_core0_head = next;
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

#define COM_UART_TX_BUF_SIZE 32
static size_t com_uart_tx_tail;
static size_t com_uart_tx_head;
static uint8_t com_uart_tx_buf[COM_UART_TX_BUF_SIZE];

// The PL011 FIFO's shadow, sized to it. com_task pops the FIFO into it
// every tick whether or not anyone reads: a byte left in the FIFO hides the
// break flag behind it and the Ctrl-C in it, and with no flow control there
// is no holding the far end, so type-ahead past this drops.
#define COM_UART_RX_BUF_SIZE 32
static size_t com_uart_rx_head;
static size_t com_uart_rx_tail;
static uint8_t com_uart_rx_buf[COM_UART_RX_BUF_SIZE];

// The source that owns the one byte past the rings -- in the handoff slot or
// the register window, both ria.c's -- so a reader by source can take it
// back. Written and read on this core's main loop only; act_loop never needs
// it.
static com_source_t com_rx_char_src;

// The length is not decoration: a read of zero bytes is a legal thing to ask
// for -- the API's short-stack pop makes read(fd, buf, 0) an ordinary 6502
// sequence -- and the buffer it hands down has no room at all. Recovering into
// it would put a byte one past the caller's buffer, and the byte is consumed
// either way, so it has to stay staged for a read that can take it.
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

// Non-consuming peek at the next byte of an SPSC RX ring (head==tail empty;
// the next byte sits one past tail). Returns the byte (0..255) or -1.
int com_ring_peek(const uint8_t *buf, size_t size, size_t head, size_t tail)
{
    if (head == tail)
        return -1;
    return buf[(tail + 1) % size];
}


// Drain the UART hw FIFO into the software ring. Scans for SIGINT
// inline so Ctrl-C is honoured even when the ring is full and the
// byte gets dropped. Called unconditionally from com_task each tick.
static void com_uart_drain_rx(void)
{
    while (uart_is_readable(COM_UART))
    {
        uint8_t c = (uint8_t)uart_get_hw(COM_UART)->dr;
        if (c == 0x03)
            ria_trigger_sigint();
        size_t next = (com_uart_rx_head + 1) % COM_UART_RX_BUF_SIZE;
        if (next == com_uart_rx_tail)
            continue;
        com_uart_rx_buf[next] = c;
        com_uart_rx_head = next;
    }
}

size_t com_uart_read(char *buf, size_t length)
{
    size_t count = 0;
    // Always pump the hw FIFO into the software ring so callers that
    // bypass com_task (e.g. vga_connect's blocking loop running only
    // mbuf_task) still see fresh bytes. Idempotent.
    com_uart_drain_rx();
    while (count < length && com_uart_rx_head != com_uart_rx_tail)
    {
        com_uart_rx_tail = (com_uart_rx_tail + 1) % COM_UART_RX_BUF_SIZE;
        buf[count++] = (char)com_uart_rx_buf[com_uart_rx_tail];
    }
    return count;
}

int com_uart_peek(void)
{
    com_uart_drain_rx();
    return com_ring_peek((const uint8_t *)com_uart_rx_buf, COM_UART_RX_BUF_SIZE,
                          com_uart_rx_head, com_uart_rx_tail);
}

// What is still in the hw FIFO was typed for the same program as what is in
// the ring, so it goes too.
void com_uart_clear(void)
{
    com_uart_drain_rx();
    com_uart_rx_head = com_uart_rx_tail = 0;
}

static bool com_uart_tx_writable(void)
{
    return (((com_uart_tx_head + 1) % COM_UART_TX_BUF_SIZE) != com_uart_tx_tail);
}

static void com_uart_tx_write(char ch)
{
    size_t next = (com_uart_tx_head + 1) % COM_UART_TX_BUF_SIZE;
    com_uart_tx_buf[next] = (uint8_t)ch;
    com_uart_tx_head = next;
}

static void com_uart_drain_tx(void)
{
    // VGA: pace one byte per TX-empty so the PIX mirror stays in sync.
    // No VGA: keep the TX FIFO topped up.
    bool vga = vga_connected();
    while (com_uart_tx_head != com_uart_tx_tail)
    {
        uint32_t fr = uart_get_hw(COM_UART)->fr;
        if (vga)
        {
            if (!(fr & UART_UARTFR_TXFE_BITS) || !pix_ready())
                break;
        }
        else if (fr & UART_UARTFR_TXFF_BITS)
            break;
        size_t next = (com_uart_tx_tail + 1) % COM_UART_TX_BUF_SIZE;
        char ch = com_uart_tx_buf[next];
        uart_putc_raw(COM_UART, ch);
        if (vga)
            pix_send(PIX_DEVICE_VGA, 0xF, 0x03, ch);
        if (ch == '\a' && com_bel_enabled)
            bel_add(&bel_teletype);
        com_uart_tx_tail = next;
    }
}

static void com_uart_flush(void)
{
    while (com_uart_tx_head != com_uart_tx_tail)
        com_uart_drain_tx();
    while (uart_get_hw(COM_UART)->fr & UART_UARTFR_BUSY_BITS)
        tight_loop_contents();
}

// One char per source per pass so the core-0 and core-1 streams interleave
// instead of one starving the other. The core-1 (6502) TX bytes come from the
// ria-owned ring via ria_uart_tx_dequeue(), which holds the consumer-side
// __dmb() pairing with the producer DMB in ria_uart_tx_write().
static void com_tx_fanout(void)
{
    while (com_uart_tx_writable() && com_telnet_tx_writable())
    {
        bool work = false;
        if (com_tx_core0_head != com_tx_core0_tail)
        {
            size_t next = (com_tx_core0_tail + 1) % COM_TX_CORE0_BUF_SIZE;
            char ch = com_tx_core0_buf[next];
            com_uart_tx_write(ch);
            com_telnet_tx_write(ch);
            com_tx_core0_tail = next;
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

// One round of TX fanout + UART RX/TX pump + telnet pump. Used by the
// stdio blocking loops so RX drain keeps up while stdout is busy; not
// re-entrant from inside com_task (which calls the same primitives).
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
    while (com_tx_core0_head != com_tx_core0_tail)
        com_stdio_pump();
    com_uart_flush();
}

/* The monitor and the startup purges still read through the SDK, which
 * wants a driver rather than a count. */
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

// Reset per-program-start console state: the BEL enable flag returns to its
// default so a program that muted it doesn't leak the setting into the next.
// The bell itself is untouched -- its queue is self-limiting and runs full
// time.
//
// Type-ahead is deliberately not reset, the byte the picker staged into the
// handoff slot included: it is console input until a 6502 reads it, and the
// ring it came out of survives a program start too. What a ready bit already
// committed to the outgoing program is in the register window, which api_run
// clears.
void com_run(void)
{
    com_bel_enabled = true;
}

void com_stop(void)
{
    if (!ria_active())
    {
        while (!ria_uart_tx_empty())
            com_stdio_pump();
        printf(STR_TERM_SOFT_RESET);
        while (!com_putchar_ready())
            com_stdio_pump();
    }
}

// Console newline for a break, skipped when the pending TX already ends
// in CRLF so we don't leave a blank line.
static void com_ensure_newline(void)
{
    size_t head = com_tx_core0_head;
    size_t tail = com_tx_core0_tail;
    size_t count = (head - tail + COM_TX_CORE0_BUF_SIZE) % COM_TX_CORE0_BUF_SIZE;
    size_t last = head;
    size_t prev = (head + COM_TX_CORE0_BUF_SIZE - 1) % COM_TX_CORE0_BUF_SIZE;
    if (count < 2 ||
        com_tx_core0_buf[last] != '\n' ||
        com_tx_core0_buf[prev] != '\r')
        putchar('\n');
}

// What was typed was meant for the program being interrupted. The byte past
// the rings is ria.c's to drop, on its own break.
void com_break(void)
{
    com_ensure_newline();
    com_rx_clear();
}

void com_task(void)
{
    // TX: drain UART buffer to hardware
    com_uart_drain_tx();

    // TX: fan out com_tx_core0_buf into UART and TEL buffers
    com_tx_fanout();

    // RX: always pump the UART hw FIFO into its software ring, so
    // bursts can back up without overflowing the tiny hw FIFO and so
    // SIGINT scans / break detection run every tick regardless of
    // whether anything downstream is consuming. keyboard and telnet have
    // their own upstream rings (keyboard_key_queue and com_telnet_rx_buf)
    // so they don't need a pump here.
    com_uart_drain_rx();

    // RX: refill the cross-core handoff (ria_uart_rx_slot, owned by ria.c),
    // only when nothing is staged in the slot or the latch. One byte per
    // tick -- bounded enough that a tight rln drain on the per-source readers
    // still wins most of the upstream bytes.
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

/* The longest caller is a monitor prompt; the UF2 progress line is shorter
 * still. Sized so neither is ever the reason a message is cut. */
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
