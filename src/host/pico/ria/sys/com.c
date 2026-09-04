/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/sys.h"
#include "core/sys/ria.h"
#include "core/str/oem.h"
#include "core/aud/bel.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/ria/regs.h"
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "ria-w/net/telnet.h"
#include "ria/sys/vga.h"
#include "ria-w/net/cyw.h"
#include "ria-w/net/wifi.h"
#include "ria/sys/cfg.h"
#include "core/str/rln.h"
#include "core/str/str.h"
#include "ria/sys/com.h"
#include "ria/sys/com_telnet.h"
#include <stdlib.h>
#include <string.h>
#include <pico/stdlib.h>
#include <pico/printf.h>
#include <pico/stdio/driver.h>
#include <hardware/sync.h>
#include <stdio.h>

#if defined(DEBUG_SYS) || defined(DEBUG_SYS_COM)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

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

#define COM_UART_TX_BUF_SIZE 32
static size_t com_uart_tx_tail;
static size_t com_uart_tx_head;
static uint8_t com_uart_tx_buf[COM_UART_TX_BUF_SIZE];

// UART RX software ring. com_task drains the hw FIFO into this ring
// every tick (so SIGINT scans and break detection keep working even
// when nobody is reading). Consumers pull via com_uart_read. Sized
// to absorb bursts that span several main-loop ticks at 115200 baud.
#define COM_UART_RX_BUF_SIZE 64
static size_t com_uart_rx_head;
static size_t com_uart_rx_tail;
static uint8_t com_uart_rx_buf[COM_UART_RX_BUF_SIZE];

// The RX handoff slot itself now lives in ria.c (ria_uart_rx_slot): act_loop on
// core 1 reads it to serve 6502 0xFFE0/0xFFE2 reads; com_task offers into it from
// the merge picker (one byte per tick when it's empty). We keep only the core-0
// source tag here — the source that owns the currently-offered byte — so
// per-source readers can recover a byte the picker offered when rln (rather than
// the 6502) is the eventual consumer. act_loop never needs the tag.
static com_source_t com_rx_char_src;

// Single-byte recover from the cross-core handoff slot. Per-source readers call
// this so a byte the merge picker offered to the 6502 isn't stranded when rln is
// the active consumer instead.
//
// The length is not decoration: a read of zero bytes is a legal thing to ask
// for -- the API's short-stack pop makes read(fd, buf, 0) an ordinary 6502
// sequence -- and the buffer it hands down has no room at all. Recovering into
// it would put a byte one past the caller's buffer, and the byte is consumed
// either way, so it has to stay in the slot for a read that can take it.
size_t com_recover_rx_char(char *buf, size_t length, com_source_t src)
{
    uint8_t ch;
    if (length && com_rx_char_src == src && ria_uart_rx_reclaim(&ch))
    {
        buf[0] = (char)ch;
        return 1;
    }
    return 0;
}

static bool com_bel_enabled = true;

// Sticky-picker dwell window: once an RX source fires it locks out the
// other sources for this many microseconds, so a single keystroke can't
// slice a paste in half. Used by com_rx_pick across the three real
// sources (keyboard, UART, telnet).
#define COM_RX_IDLE_US 1000

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

static size_t com_uart_read(char *buf, size_t length)
{
    size_t count = com_recover_rx_char(buf, length, COM_SOURCE_UART);
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

static int com_uart_peek(void)
{
    com_uart_drain_rx();
    return com_ring_peek((const uint8_t *)com_uart_rx_buf, COM_UART_RX_BUF_SIZE,
                          com_uart_rx_head, com_uart_rx_tail);
}

// Local keyboard input. Steals the cross-core handoff slot if it was
// tagged KEYBOARD, then reads from keymap_in_chars. No internal sticky
// dwell — the outer com_rx_pick holds against the other sources at
// the 1 ms grain.
static size_t com_keyboard_read(char *buf, size_t length)
{
    size_t count = com_recover_rx_char(buf, length, COM_SOURCE_KEYBOARD);
    if (count < length)
        count += keymap_in_chars(&buf[count], length - count);
    return count;
}

// Dispatch a read to the per-source reader. COM_SOURCE_ANY returns 0.
static size_t com_read_source(com_source_t src, char *buf, size_t length)
{
    switch (src)
    {
    case COM_SOURCE_KEYBOARD:
        return com_keyboard_read(buf, length);
    case COM_SOURCE_UART:
        return com_uart_read(buf, length);
    case COM_SOURCE_TEL:
        return com_telnet_read(buf, length);
    case COM_SOURCE_ANY:
        break;
    }
    return 0;
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

// Sticky three-source multiplex: keyboard, UART, telnet. Whichever
// fires first holds the lock until idle for 1ms, so a single tap on
// one source can't slice a paste on another. Used by stdin and by
// com_task's RX handoff refill. *src_out, when non-NULL, reports
// which source produced the returned bytes so com_task can tag the
// offered byte for later recovery by the matching per-source reader.
static size_t com_rx_pick(char *buf, size_t length, com_source_t *src_out)
{
    static com_source_t source = COM_SOURCE_ANY;
    static absolute_time_t idle_timer;

    if (source != COM_SOURCE_ANY && time_reached(idle_timer))
        source = COM_SOURCE_ANY;

    if (source == COM_SOURCE_KEYBOARD || source == COM_SOURCE_ANY)
    {
        size_t i = com_keyboard_read(buf, length);
        if (i)
        {
            source = COM_SOURCE_KEYBOARD;
            idle_timer = make_timeout_time_us(COM_RX_IDLE_US);
            if (src_out)
                *src_out = COM_SOURCE_KEYBOARD;
            return i;
        }
        // Keyboard doesn't hold the lock when empty.
        source = COM_SOURCE_ANY;
    }

    if (source == COM_SOURCE_UART || source == COM_SOURCE_ANY)
    {
        size_t i = com_uart_read(buf, length);
        if (i)
        {
            source = COM_SOURCE_UART;
            idle_timer = make_timeout_time_us(COM_RX_IDLE_US);
            if (src_out)
                *src_out = COM_SOURCE_UART;
            return i;
        }
    }

    if (source == COM_SOURCE_TEL || source == COM_SOURCE_ANY)
    {
        size_t i = com_telnet_read(buf, length);
        if (i)
        {
            source = COM_SOURCE_TEL;
            idle_timer = make_timeout_time_us(COM_RX_IDLE_US);
            if (src_out)
                *src_out = COM_SOURCE_TEL;
            return i;
        }
    }

    return 0;
}

// Single-byte reader.
//
// Explicit single-source pull (*src set on entry) reads only from that
// source — used by rln to finish off in-flight ESC tails during a
// deferred completion without consuming bytes from clean sources.
//
// Any-source pull (*src == COM_SOURCE_ANY on entry) picks the next
// byte via the sticky-source RX picker; on a byte, *src is set to the
// delivering source.
int com_getchar(com_source_t *src)
{
    if (src && *src != COM_SOURCE_ANY)
    {
        char ch;
        if (com_read_source(*src, &ch, 1))
            return (unsigned char)ch;
        *src = COM_SOURCE_ANY;
        return PICO_ERROR_TIMEOUT;
    }

    char ch;
    com_source_t picked;
    if (com_rx_pick(&ch, 1, &picked))
    {
        if (src)
            *src = picked;
        return (unsigned char)ch;
    }
    if (src)
        *src = COM_SOURCE_ANY;
    return PICO_ERROR_TIMEOUT;
}

// Non-consuming 1-byte peek at a specific source. Mirrors com_getchar's
// single-source path (recover slot, then the source FIFO) without advancing.
// Only the tracked terminal sources (UART/TEL) are peekable; others report
// none. rln uses this during a deferred completion to tell an in-flight
// protocol reply (begins with ESC) from the next pasted line's typed bytes.
int com_peekchar(com_source_t src)
{
    if (com_rx_char_src == src)
    {
        int ch = ria_uart_rx_peek();
        if (ch >= 0)
            return ch;
    }
    switch (src)
    {
    case COM_SOURCE_UART:
        return com_uart_peek();
    case COM_SOURCE_TEL:
        return com_telnet_peek();
    default:
        return -1;
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

size_t com_stdin_read(char *buf, size_t length)
{
    size_t count = 0;

    // Take char from RIA register. Only with somewhere to put it: a zero-byte
    // read must leave the staged byte staged, not drop it on the floor -- and
    // not write it past the end of a buffer that has no room.
    if (count < length && (REGS(0xFFE0) & 0b01000000))
    {
        buf[count++] = REGS(0xFFE2);
        REGS(0xFFE0) = 0;
        REGS(0xFFE2) = 0;
    }

    // Pick up new chars via the sticky merge picker. A reader here sees a
    // flat byte stream — the source tag is irrelevant. The per-source
    // readers inside com_rx_pick recover the offered byte when tagged for
    // their source, so any byte sitting in the handoff slot is delivered
    // here without a separate drain.
    if (count < length)
        count += com_rx_pick(&buf[count], length - count, NULL);

    return count;
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

// Reset per-program-start console state: the BEL alert returns to its default
// (enabled) so a program that muted it doesn't leak the setting into the next.
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

void com_break(void)
{
    com_ensure_newline();

    // Drain hw FIFO first so any in-flight bytes land in the ring,
    // then clear the ring.
    com_uart_drain_rx();
    com_uart_rx_head = com_uart_rx_tail = 0;

    char scratch[16];
    while (keymap_in_chars(scratch, sizeof scratch))
        ;

#ifdef RP6502_RIA_W
    if (com_telnet_connected())
        while (telnet_rx(NET_TELNET_DESC, scratch, sizeof scratch))
            ;
    com_telnet_clear_rx();
#endif

    REGS(0xFFE0) = 0;
    REGS(0xFFE2) = 0;
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

    // RX: refill the cross-core handoff slot (ria_uart_rx_slot, owned by ria.c).
    // act_loop on core 1 only ever observes -1 or 0..255 and never reads
    // com_rx_char_src. The __dmb() here is a core-0 compiler barrier (memory
    // clobber) pinning the non-volatile com_rx_char_src write ahead of the slot
    // store, so a per-source reader on core 0 can't see a fresh byte tagged with
    // a stale src. One byte per tick — bounded enough that a tight rln drain on
    // the per-source readers still wins most of the upstream bytes.
    if (ria_uart_rx_offer_ready())
    {
        char ch;
        com_source_t src;
        if (com_rx_pick(&ch, 1, &src))
        {
            com_rx_char_src = src;
            __dmb();
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
