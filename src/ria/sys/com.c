/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "aud/bel.h"
#include "hid/kbd.h"
#include "sys/com.h"
#include "sys/mem.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "net/tel.h"
#include "sys/vga.h"
#include "net/cyw.h"
#include "net/wfi.h"
#include "sys/cfg.h"
#include "str/rln.h"
#include "str/str.h"
#include <stdlib.h>
#include <string.h>
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>
#include <hardware/sync.h>
#include <stdio.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_COM)
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

volatile uint8_t com_tx_core0_buf[COM_TX_CORE0_BUF_SIZE];
volatile size_t com_tx_core0_head;
volatile size_t com_tx_core0_tail;

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
static size_t com_recover_rx_char(char *buf, com_source_t src)
{
    uint8_t ch;
    if (com_rx_char_src == src && ria_uart_rx_reclaim(&ch))
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
// sources (kbd, UART, telnet).
#define COM_RX_IDLE_US 1000

// Non-consuming peek at the next byte of an SPSC RX ring (head==tail empty;
// the next byte sits one past tail). Returns the byte (0..255) or -1.
static int com_ring_peek(const uint8_t *buf, size_t size, size_t head, size_t tail)
{
    if (head == tail)
        return -1;
    return buf[(tail + 1) % size];
}

#ifndef RP6502_RIA_W

static bool com_tel_tx_writable(void) { return true; }
static void com_tel_tx_write(char ch) { (void)ch; }
static size_t com_tel_read(char *buf, size_t length)
{
    (void)buf;
    (void)length;
    return 0;
}
static int com_tel_peek(void) { return -1; }
static void com_tel_pump(void) {}
static void com_tel_task(void) {}

#else

#define COM_TEL_KEY_SIZE 33
static uint16_t com_tel_port = 23;
static char com_tel_key[COM_TEL_KEY_SIZE];

typedef enum
{
    COM_TEL_STATE_IDLE,
    COM_TEL_STATE_LISTENING,
    COM_TEL_STATE_AUTH,
    COM_TEL_STATE_CONNECTED,
} com_tel_state_t;
static com_tel_state_t com_tel_state = COM_TEL_STATE_IDLE;
static uint16_t com_tel_active_port;

static char com_tel_auth_buf[COM_TEL_KEY_SIZE];
static uint8_t com_tel_auth_len;

#define COM_TEL_TX_BUF_SIZE 32
static char com_tel_tx_buf[COM_TEL_TX_BUF_SIZE];
static volatile size_t com_tel_tx_head;
static volatile size_t com_tel_tx_tail;

#define COM_TEL_RX_BUF_SIZE 32
// After this many milliseconds with a full ring and no consume,
// com_tel_drain_rx drops bytes instead of backpressuring the TCP peer.
#define COM_TEL_RX_OVERFLOW_MS 5000
static char com_tel_rx_buf[COM_TEL_RX_BUF_SIZE];
static size_t com_tel_rx_head;
static size_t com_tel_rx_tail;
static absolute_time_t com_tel_rx_drop_after;

static void com_tel_clear_rx(void)
{
    com_tel_rx_head = com_tel_rx_tail = 0;
    com_tel_rx_drop_after = make_timeout_time_ms(COM_TEL_RX_OVERFLOW_MS);
}

static void com_tel_clear_rings(void)
{
    com_tel_tx_head = com_tel_tx_tail = 0;
    com_tel_clear_rx();
}

static bool com_tel_tx_writable(void)
{
    return ((com_tel_tx_head + 1) % COM_TEL_TX_BUF_SIZE) != com_tel_tx_tail;
}

static void com_tel_tx_write(char ch)
{
    com_tel_tx_head = (com_tel_tx_head + 1) % COM_TEL_TX_BUF_SIZE;
    com_tel_tx_buf[com_tel_tx_head] = ch;
}

static size_t com_tel_read(char *buf, size_t length)
{
    size_t count = com_recover_rx_char(buf, COM_SOURCE_TEL);
    while (count < length && com_tel_rx_head != com_tel_rx_tail)
    {
        com_tel_rx_tail = (com_tel_rx_tail + 1) % COM_TEL_RX_BUF_SIZE;
        buf[count++] = com_tel_rx_buf[com_tel_rx_tail];
    }
    if (count)
        com_tel_rx_drop_after = make_timeout_time_ms(COM_TEL_RX_OVERFLOW_MS);
    return count;
}

static int com_tel_peek(void)
{
    return com_ring_peek((const uint8_t *)com_tel_rx_buf, COM_TEL_RX_BUF_SIZE,
                         com_tel_rx_head, com_tel_rx_tail);
}

static void com_tel_drain_tx(void)
{
    if (com_tel_state != COM_TEL_STATE_CONNECTED)
    {
        // Discard — nobody to send to
        com_tel_tx_tail = com_tel_tx_head;
        return;
    }
    if (com_tel_tx_tail == com_tel_tx_head)
        return;
    size_t start = (com_tel_tx_tail + 1) % COM_TEL_TX_BUF_SIZE;
    size_t len;
    if (com_tel_tx_head >= start)
        len = com_tel_tx_head - start + 1;
    else
        len = COM_TEL_TX_BUF_SIZE - start;
    uint16_t sent = tel_tx(SYS_TEL_DESC, &com_tel_tx_buf[start], len);
    com_tel_tx_tail = (com_tel_tx_tail + sent) % COM_TEL_TX_BUF_SIZE;
}

// Only drives lwIP via cyw_task() when the ring is full and the upstream
// would otherwise stall — cyw_task synchronously fires
// com_tel_on_accept/on_disconnect callbacks and mutates com_tel_state +
// the rings, so callers must re-check state after return.
static void com_tel_pump(void)
{
    com_tel_drain_tx();
    if (!com_tel_tx_writable())
        cyw_task();
}

static void com_tel_handle_auth(uint8_t ch)
{
    if (ch == '\b' || ch == 127)
    {
        if (com_tel_auth_len > 0)
        {
            com_tel_auth_len--;
            tel_tx(SYS_TEL_DESC, "\b \b", 3);
        }
    }
    else if (ch == '\r' || ch == '\n')
    {
        com_tel_auth_buf[com_tel_auth_len] = 0;
        if (strcmp(com_tel_auth_buf, com_tel_key) == 0)
        {
            tel_tx(SYS_TEL_DESC, STR_TEL_CONNECTED, STR_TEL_CONNECTED_LEN);
            com_tel_state = COM_TEL_STATE_CONNECTED;
            vga_set_tel_console_active(true);
            DBG("NET TEL console authenticated\n");
        }
        else
        {
            tel_tx(SYS_TEL_DESC, STR_TEL_ACCESS_DENIED, STR_TEL_ACCESS_DENIED_LEN);
            DBG("NET TEL console auth failed\n");
            com_tel_state = COM_TEL_STATE_LISTENING;
            tel_close(SYS_TEL_DESC);
        }
    }
    else if (ch >= 32 && ch < 127 && com_tel_auth_len < COM_TEL_KEY_SIZE - 1)
    {
        com_tel_auth_buf[com_tel_auth_len++] = ch;
        tel_tx(SYS_TEL_DESC, "*", 1);
    }
}

static void com_tel_drain_rx(void)
{
    // Default: limit read to ring buffer free space so decoded bytes
    // always fit (decoded <= raw). If the ring has been full and the
    // consumer has been idle for COM_TEL_RX_OVERFLOW_MS, switch to
    // drop-mode: drain a full scratch buffer from tel_rx and discard,
    // but still scan discarded bytes for Ctrl-C so a SIGINT during
    // overflow is not lost.
    uint16_t limit = COM_TEL_RX_BUF_SIZE;
    bool drop_mode = false;
    if (com_tel_state == COM_TEL_STATE_CONNECTED)
    {
        size_t used = (com_tel_rx_head - com_tel_rx_tail + COM_TEL_RX_BUF_SIZE) % COM_TEL_RX_BUF_SIZE;
        size_t free = COM_TEL_RX_BUF_SIZE - 1 - used;
        if (free == 0)
        {
            if (!time_reached(com_tel_rx_drop_after))
                return;
            drop_mode = true;
        }
        else
            limit = (uint16_t)free;
    }

    char decoded[COM_TEL_RX_BUF_SIZE];
    uint16_t decoded_len = tel_rx(SYS_TEL_DESC, decoded, limit);

    for (uint16_t i = 0; i < decoded_len; i++)
    {
        uint8_t ch = (uint8_t)decoded[i];
        if (com_tel_state == COM_TEL_STATE_AUTH)
        {
            com_tel_handle_auth(ch);
            if (com_tel_state != COM_TEL_STATE_AUTH)
                return;
        }
        else if (com_tel_state == COM_TEL_STATE_CONNECTED)
        {
            if (ch == 0x03)
                ria_trigger_sigint();
            if (drop_mode)
                continue;
            com_tel_rx_head = (com_tel_rx_head + 1) % COM_TEL_RX_BUF_SIZE;
            com_tel_rx_buf[com_tel_rx_head] = ch;
        }
    }

    // NAWS arrives as a side effect of the tel_rx decode above; relay any
    // fresh size to rln, which reflows the line in place on a resize.
    if (com_tel_state == COM_TEL_STATE_CONNECTED)
    {
        uint16_t nw, nh;
        if (tel_get_naws(SYS_TEL_DESC, &nw, &nh))
            rln_set_naws_size(nw, nh);
    }
}

static bool com_tel_should_listen(void)
{
    return com_tel_port > 0 && com_tel_key[0] != 0 && wfi_ready();
}

// Unified teardown for both full shutdown (target=IDLE, closes the
// listen socket and the session pcb via SYS_TEL_DESC) and peer-driven
// disconnect (target=LISTENING, keeps the listener armed; the session
// pcb close is done by the caller with its own desc). Both targets
// clear the rings and assign the new state.
static void com_tel_teardown(com_tel_state_t target)
{
    bool was_session = (com_tel_state == COM_TEL_STATE_AUTH || com_tel_state == COM_TEL_STATE_CONNECTED);
    bool was_connected = (com_tel_state == COM_TEL_STATE_CONNECTED);
    if (was_session && target == COM_TEL_STATE_IDLE)
        tel_close(SYS_TEL_DESC);
    if (was_connected && target != COM_TEL_STATE_CONNECTED)
    {
        vga_set_tel_console_active(false);
        rln_set_naws_size(0, 0); // drop stale telnet geometry
    }
    if (target == COM_TEL_STATE_IDLE && com_tel_state != COM_TEL_STATE_IDLE)
    {
        tel_listen_close(com_tel_active_port);
        com_tel_active_port = 0;
    }
    com_tel_state = target;
    com_tel_clear_rings();
}

static void com_tel_shutdown(void)
{
    com_tel_teardown(COM_TEL_STATE_IDLE);
}

static void com_tel_on_disconnect(int desc)
{
    if (com_tel_state == COM_TEL_STATE_AUTH || com_tel_state == COM_TEL_STATE_CONNECTED)
    {
        DBG("NET TEL console disconnected\n");
        com_tel_teardown(COM_TEL_STATE_LISTENING);
    }
    tel_close(desc);
}

static bool com_tel_on_accept(uint16_t port)
{
    if (com_tel_state != COM_TEL_STATE_LISTENING)
        return false;

    if (!tel_accept_server(SYS_TEL_DESC, port, com_tel_on_disconnect))
        return false;

    tel_tx(SYS_TEL_DESC, STR_TEL_PASSKEY, STR_TEL_PASSKEY_LEN);

    com_tel_auth_len = 0;
    com_tel_clear_rings();
    com_tel_state = COM_TEL_STATE_AUTH;
    DBG("NET TEL console accepted, awaiting auth\n");
    return true;
}

void com_tel_load_port(const char *str)
{
    str_parse_uint16(&str, &com_tel_port);
}

void com_tel_load_key(const char *str)
{
    size_t n = strlen(str);
    if (n < COM_TEL_KEY_SIZE)
    {
        memcpy(com_tel_key, str, n);
        com_tel_key[n] = 0;
    }
}

bool com_tel_set_port(uint16_t port)
{
    if (com_tel_port != port)
    {
        com_tel_port = port;
        if (port == 0)
            com_tel_shutdown();
        cfg_save();
    }
    return true;
}

bool com_tel_set_key(const char *key)
{
    size_t n = strlen(key);
    if (n >= COM_TEL_KEY_SIZE)
        return false;
    if (strcmp(com_tel_key, key))
    {
        memcpy(com_tel_key, key, n);
        com_tel_key[n] = 0;
        if (com_tel_key[0] == 0)
            com_tel_shutdown();
        cfg_save();
    }
    return true;
}

uint16_t com_tel_get_port(void)
{
    return com_tel_port;
}

const char *com_tel_get_key(void)
{
    return com_tel_key;
}

static void com_tel_task(void)
{
    com_tel_drain_tx();

    if (com_tel_state != COM_TEL_STATE_IDLE &&
        (!com_tel_should_listen() || com_tel_active_port != com_tel_port))
    {
        com_tel_shutdown();
        return;
    }

    switch (com_tel_state)
    {
    case COM_TEL_STATE_IDLE:
        if (com_tel_should_listen() && tel_listen(com_tel_port, com_tel_on_accept))
        {
            com_tel_active_port = com_tel_port;
            com_tel_state = COM_TEL_STATE_LISTENING;
            DBG("NET TEL console listening on port %u\n", com_tel_port);
        }
        break;
    case COM_TEL_STATE_AUTH:
    case COM_TEL_STATE_CONNECTED:
        com_tel_drain_rx();
        break;
    case COM_TEL_STATE_LISTENING:
        break;
    }
}

#endif

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
    size_t count = com_recover_rx_char(buf, COM_SOURCE_UART);
    // Always pump the hw FIFO into the software ring so callers that
    // bypass com_task (e.g. vga_connect's blocking loop running only
    // mem_task) still see fresh bytes. Idempotent.
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
// tagged KBD, then reads from kbd_stdio_in_chars. No internal sticky
// dwell — the outer com_rx_pick holds against the other sources at
// the 1 ms grain.
static size_t com_kbd_read(char *buf, size_t length)
{
    size_t count = com_recover_rx_char(buf, COM_SOURCE_KBD);
    if (count < length)
        count += kbd_stdio_in_chars(&buf[count], length - count);
    return count;
}

// Dispatch a read to the per-source reader. COM_SOURCE_ANY returns 0.
static size_t com_read_source(com_source_t src, char *buf, size_t length)
{
    switch (src)
    {
    case COM_SOURCE_KBD:
        return com_kbd_read(buf, length);
    case COM_SOURCE_UART:
        return com_uart_read(buf, length);
    case COM_SOURCE_TEL:
        return com_tel_read(buf, length);
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
    while (com_uart_tx_writable() && com_tel_tx_writable())
    {
        bool work = false;
        if (com_tx_core0_head != com_tx_core0_tail)
        {
            size_t next = (com_tx_core0_tail + 1) % COM_TX_CORE0_BUF_SIZE;
            char ch = com_tx_core0_buf[next];
            com_uart_tx_write(ch);
            com_tel_tx_write(ch);
            com_tx_core0_tail = next;
            work = true;
            if (!com_uart_tx_writable() || !com_tel_tx_writable())
                break;
        }
        uint8_t ch1;
        if (ria_uart_tx_dequeue(&ch1))
        {
            com_uart_tx_write((char)ch1);
            com_tel_tx_write((char)ch1);
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

    if (source == COM_SOURCE_KBD || source == COM_SOURCE_ANY)
    {
        size_t i = com_kbd_read(buf, length);
        if (i)
        {
            source = COM_SOURCE_KBD;
            idle_timer = make_timeout_time_us(COM_RX_IDLE_US);
            if (src_out)
                *src_out = COM_SOURCE_KBD;
            return i;
        }
        // Kbd doesn't hold the lock when empty.
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
        size_t i = com_tel_read(buf, length);
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
        return com_tel_peek();
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
    com_tel_pump();
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

static int com_stdio_in_chars(char *buf, int length)
{
    int count = 0;

    // Take char from RIA register
    if (REGS(0xFFE0) & 0b01000000)
    {
        buf[count++] = REGS(0xFFE2);
        REGS(0xFFE0) = 0;
        REGS(0xFFE2) = 0;
    }

    // Pick up new chars via the sticky merge picker. stdio sees a
    // flat byte stream — the source tag is irrelevant here. The
    // per-source readers inside com_rx_pick recover the offered byte
    // when tagged for their source, so any byte sitting in the handoff
    // slot is delivered here without a separate drain.
    count += com_rx_pick(&buf[count], length - count, NULL);

    return count ? count : PICO_ERROR_NO_DATA;
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

void com_stop(void)
{
    if (!ria_active())
    {
        printf(STR_TERM_SOFT_RESET);
        while (!com_putchar_ready())
            com_stdio_pump();
        putchar('\n'); // doesn't flush
    }
}

void com_break(void)
{
    // Queue a newline unless com_stop just left a CRLF.
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

    // Drain hw FIFO first so any in-flight bytes land in the ring,
    // then clear the ring.
    com_uart_drain_rx();
    com_uart_rx_head = com_uart_rx_tail = 0;

    char scratch[16];
    while (kbd_stdio_in_chars(scratch, sizeof scratch))
        ;

#ifdef RP6502_RIA_W
    if (com_tel_state == COM_TEL_STATE_CONNECTED)
        while (tel_rx(SYS_TEL_DESC, scratch, sizeof scratch))
            ;
    com_tel_clear_rx();
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
    // whether anything downstream is consuming. kbd and telnet have
    // their own upstream rings (kbd_key_queue and com_tel_rx_buf)
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
        main_break();
    break_detect = current_break;

    com_tel_task();
}

bool com_get_bel(void)
{
    return com_bel_enabled;
}

void com_set_bel(bool value)
{
    com_bel_enabled = value;
}
