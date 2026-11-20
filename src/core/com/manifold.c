/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/manifold.h"
#include "core/com/com.h"
#include "core/com/manifold_tx.h"
#include "core/aud/bel.h"
#include "core/sys/ria.h"
#include "core/sys/ring.h"
#include "core/sys/timer.h"
#include "machine.h"

#include <unistd.h>

#define MANIFOLD_ETX 0x03

typedef RING(uint8_t, MANIFOLD_RING_SIZE) manifold_ring_t;

static manifold_ring_t manifold_keyboard_ring;
static manifold_ring_t manifold_uart_ring;

static char manifold_crlf_last;

static int (*manifold_term_out)(const char *buf, int len, bool force);
static void (*manifold_tx_tap)(const char *buf, int len);
static void (*manifold_std_tap)(int fd, const char *buf, int len);
static void (*manifold_stderr_sink)(const char *buf, int len);

static void (*manifold_wire_tx)(const char *buf, int len);
static size_t (*manifold_wire_rx)(char *buf, size_t max);
static bool manifold_wire_stream;
static bool manifold_wire_held;
static timer_mach_t manifold_wire_hold;

/* The count, then MANIFOLD_RING_SIZE bytes oldest first. */
static void manifold_ring_save(sst_cursor_t *c, manifold_ring_t *r)
{
    const uint32_t len = RING_LEN(r);
    sst_put_u16(c, (uint16_t)len);
    for (uint32_t i = 0; i < MANIFOLD_RING_SIZE; i++)
        sst_put_u8(c, i < len ? *RING_AT(r, i) : 0);
}

static uint16_t manifold_ring_get(sst_cursor_t *c, uint8_t *buf)
{
    const uint16_t len = sst_get_u16(c);
    sst_get(c, buf, MANIFOLD_RING_SIZE);
    return len;
}

static void manifold_ring_set(manifold_ring_t *r, const uint8_t *buf, uint16_t len)
{
    RING_RESET(r);
    RING_IN(r, buf, len);
}

void manifold_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    manifold_ring_save(c, &manifold_keyboard_ring);
    manifold_ring_save(c, &manifold_uart_ring);
    sst_put_u8(c, (uint8_t)manifold_crlf_last);
    manifold_tx_sst_save(c);
}

bool manifold_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint8_t kb[MANIFOLD_RING_SIZE], wire[MANIFOLD_RING_SIZE];
    const uint16_t kb_len = manifold_ring_get(c, kb);
    const uint16_t wire_len = manifold_ring_get(c, wire);
    uint8_t crlf = sst_get_u8(c);
    if (!manifold_tx_sst_load(c) || !sst_ok(c) || kb_len > MANIFOLD_RING_SIZE ||
        wire_len > MANIFOLD_RING_SIZE)
        return false;
    manifold_ring_set(&manifold_keyboard_ring, kb, kb_len);
    manifold_ring_set(&manifold_uart_ring, wire, wire_len);
    manifold_crlf_last = (char)crlf;
    return true;
}

size_t manifold_keyboard_read(char *buf, size_t length)
{
    return RING_OUT(&manifold_keyboard_ring, buf, (uint32_t)length);
}

int manifold_keyboard_peek(size_t offset)
{
    return offset < RING_LEN(&manifold_keyboard_ring)
               ? *RING_AT(&manifold_keyboard_ring, offset)
               : -1;
}

void manifold_keyboard_clear(void)
{
    RING_SKIP(&manifold_keyboard_ring, RING_LEN(&manifold_keyboard_ring));
}

size_t manifold_uart_read(char *buf, size_t length)
{
    return RING_OUT(&manifold_uart_ring, buf, (uint32_t)length);
}

int manifold_uart_peek(size_t offset)
{
    return offset < RING_LEN(&manifold_uart_ring)
               ? *RING_AT(&manifold_uart_ring, offset)
               : -1;
}

void manifold_uart_clear(void)
{
    RING_SKIP(&manifold_uart_ring, RING_LEN(&manifold_uart_ring));
}

void manifold_set_term_out(int (*out_chars)(const char *buf, int len, bool force))
{
    manifold_term_out = out_chars;
}

void manifold_set_tx_tap(void (*tap)(const char *buf, int len))
{
    manifold_tx_tap = tap;
}

void manifold_set_std_tap(void (*tap)(int fd, const char *buf, int len))
{
    manifold_std_tap = tap;
}

void manifold_set_stderr_sink(void (*sink)(const char *buf, int len))
{
    manifold_stderr_sink = sink;
}

void manifold_set_wire(void (*tx)(const char *buf, int len),
                       size_t (*rx)(char *buf, size_t max), bool stream)
{
    manifold_wire_tx = tx;
    manifold_wire_rx = rx;
    manifold_wire_stream = stream;
    manifold_wire_held = false;
}

/* The UART is the only terminal here besides term.c. The UART's sinks are the
 * tx tap, the host wire and term.c; COM_TERM_SCREEN's is term.c alone. The
 * terminal takes bytes until one has to wait for a smooth scroll still
 * moving, unless force, and the other sinks get only what it took, so every
 * terminal shows the same output at the same time. Returns how many of buf
 * went out. */
static size_t manifold_emit(uint8_t to, const char *buf, size_t len, bool force)
{
    size_t took = len;
    if ((to & (COM_TERM_UART | COM_TERM_SCREEN)) && manifold_term_out)
        took = (size_t)manifold_term_out(buf, (int)len, force);
    if ((to & COM_TERM_UART) && took)
    {
        if (manifold_tx_tap)
            manifold_tx_tap(buf, (int)took);
        if (manifold_wire_tx)
            manifold_wire_tx(buf, (int)took);
    }
    return took;
}

/* What waits goes out, oldest first and in runs for the same terminals, as
 * far as the terminal takes it. With force, a byte the terminal holds goes
 * out all the same, which ends the scroll it waits for at once. */
static void manifold_drain(bool force)
{
    size_t len;
    while ((len = manifold_tx_len()))
    {
        char run[MANIFOLD_TX_SIZE];
        const uint8_t to = manifold_tx_at(0).to;
        size_t n = 0;
        for (; n < len; n++)
        {
            manifold_tx_entry_t e = manifold_tx_at(n);
            if (e.to != to)
                break;
            run[n] = (char)e.ch;
        }
        size_t sent = manifold_emit(to, run, n, force);
        manifold_tx_skip(sent);
        if (sent < n)
            return;
    }
}

/* A write from inside a task cannot wait for frames, so a byte that must go
 * in ends a smooth scroll it would wait for. */
void manifold_tx_make_room(void)
{
    manifold_drain(true);
}

/* Output is written as it is made, as far as the terminal takes it. */
void manifold_tx_wrote(void)
{
    manifold_drain(false);
}

/* A newline that does not follow a CR goes in as CR LF, and what waits goes
 * out at once as far as it can. */
static void manifold_crlf_put(uint8_t to, char c)
{
    if (c == '\n' && manifold_crlf_last != '\r')
        manifold_tx_put(to, '\r');
    manifold_tx_put(to, c);
    manifold_crlf_last = c;
}

void manifold_putchar_raw(char c)
{
    if (!manifold_tx_try_put(COM_TERM_ALL, c))
        return;
    if (manifold_std_tap)
        manifold_std_tap(STDOUT_FILENO, &c, 1);
    manifold_tx_wrote();
}

int manifold_putchar(int c)
{
    manifold_crlf_put(COM_TERM_ALL, (char)c);
    manifold_tx_wrote();
    return (int)(unsigned char)c;
}

/* Writes what fits and returns the count, as a Pico does. */
size_t manifold_write(int fd, const char *buf, size_t n)
{
    size_t i = 0;
    for (; i < n && manifold_cts(); i++)
        manifold_crlf_put(COM_TERM_ALL, buf[i]);
    manifold_tx_wrote();
    if (manifold_std_tap)
        manifold_std_tap(fd, buf, (int)i);
    if (fd == STDERR_FILENO && manifold_stderr_sink)
        manifold_stderr_sink(buf, (int)i);
    return i;
}

/* What waits goes out now, ending a smooth scroll it waits for: the machine
 * cannot run frames until the scroll is over while a task waits here. */
void manifold_flush(void)
{
    manifold_drain(true);
}

bool manifold_drained(void)
{
    return !manifold_tx_len();
}

/* The emulated terminal is the console's own, so com takes its answers. */
void manifold_reply(const char *s, size_t n)
{
    com_reply(s, n);
}

void manifold_bell(void)
{
    if (com_get_bel())
        bel_add(&bel_teletype);
}

/* A Ctrl-C is scanned for before the bytes are pushed, so a break is seen
 * even when the ring is full and the bytes are dropped. */
static void manifold_scan_break(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == MANIFOLD_ETX)
            ria_trigger_sigint();
}

void manifold_keyboard_push(const char *s, size_t n)
{
    manifold_scan_break(s, n);
    if (n <= RING_AVAIL(&manifold_keyboard_ring))
        RING_IN(&manifold_keyboard_ring, s, (uint32_t)n);
}

void manifold_uart_push(const char *s, size_t n)
{
    manifold_scan_break(s, n);
    RING_IN(&manifold_uart_ring, s, (uint32_t)n);
}

size_t manifold_keyboard_free(void)
{
    return RING_AVAIL(&manifold_keyboard_ring);
}

/* A stream wire is not read while the ring is full. A console wire is left for
 * COM_HOLD_MS and then read into the full ring, where the bytes are dropped,
 * because a Ctrl-C typed behind the type-ahead has to be seen even though the
 * rest of what was typed is lost. */
void manifold_task(void)
{
    manifold_drain(false);
    if (!manifold_wire_rx)
        return;
    char buf[MANIFOLD_RING_SIZE];
    size_t room = RING_AVAIL(&manifold_uart_ring);
    if (room)
    {
        manifold_wire_held = false;
        size_t n = manifold_wire_rx(buf, room < sizeof buf ? room : sizeof buf);
        if (!n)
            return;
        if (manifold_wire_stream)
            RING_IN(&manifold_uart_ring, buf, (uint32_t)n);
        else
            manifold_uart_push(buf, n);
        return;
    }
    if (manifold_wire_stream)
        return;
    if (!manifold_wire_held)
    {
        manifold_wire_held = true;
        manifold_wire_hold = timer_mach_in_ms(COM_HOLD_MS);
    }
    if (!timer_mach_passed(manifold_wire_hold))
        return;
    size_t n = manifold_wire_rx(buf, sizeof buf);
    if (n)
        manifold_uart_push(buf, n);
}
