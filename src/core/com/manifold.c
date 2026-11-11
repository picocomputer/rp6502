/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/manifold.h"
#include "core/com/com.h"
#include "core/aud/bel.h"
#include "core/sys/ria.h"
#include "core/sys/ring.h"
#include "core/sys/timer.h"
#include "machine.h"

#include <string.h>
#include <unistd.h>

#define MANIFOLD_ETX 0x03

typedef RING(uint8_t, MANIFOLD_RING_SIZE) manifold_ring_t;

static manifold_ring_t manifold_keyboard_ring;
static manifold_ring_t manifold_uart_ring;
static uint8_t manifold_reply_buf[32];
static size_t manifold_reply_len;
static bool manifold_reply_suppressed;

static char manifold_crlf_last;

static void (*manifold_term_out)(const char *buf, int len);
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
    sst_put_u8(c, (uint8_t)manifold_reply_len);
    sst_put(c, manifold_reply_buf, sizeof manifold_reply_buf);
    sst_put_u8(c, (uint8_t)manifold_crlf_last);
}

bool manifold_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint8_t kb[MANIFOLD_RING_SIZE], wire[MANIFOLD_RING_SIZE];
    const uint16_t kb_len = manifold_ring_get(c, kb);
    const uint16_t wire_len = manifold_ring_get(c, wire);
    uint8_t len = sst_get_u8(c);
    uint8_t held[sizeof manifold_reply_buf];
    sst_get(c, held, sizeof held);
    uint8_t crlf = sst_get_u8(c);
    if (!sst_ok(c) || kb_len > MANIFOLD_RING_SIZE || wire_len > MANIFOLD_RING_SIZE ||
        len > sizeof manifold_reply_buf)
        return false;
    manifold_ring_set(&manifold_keyboard_ring, kb, kb_len);
    manifold_ring_set(&manifold_uart_ring, wire, wire_len);
    manifold_reply_len = len;
    memcpy(manifold_reply_buf, held, sizeof manifold_reply_buf);
    manifold_crlf_last = (char)crlf;
    return true;
}

/* A reply is held back while the ring still has bytes in it, because those
 * bytes can be the start of a sequence whose rest has not arrived yet, and
 * the reply would then be delivered in the middle of that sequence. */
static void manifold_reply_promote(void)
{
    if (!manifold_reply_len || RING_LEN(&manifold_uart_ring))
        return;
    RING_IN(&manifold_uart_ring, manifold_reply_buf, (uint32_t)manifold_reply_len);
    manifold_reply_len = 0;
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
    size_t n = 0;
    while (n < length)
    {
        manifold_reply_promote();
        const uint32_t got = RING_OUT(&manifold_uart_ring, buf + n, (uint32_t)(length - n));
        if (!got)
            break;
        n += got;
    }
    return n;
}

/* A held reply comes after the ring, as manifold_uart_read delivers it. */
int manifold_uart_peek(size_t offset)
{
    manifold_reply_promote();
    const uint32_t len = RING_LEN(&manifold_uart_ring);
    if (offset < len)
        return *RING_AT(&manifold_uart_ring, offset);
    offset -= len;
    return offset < manifold_reply_len ? manifold_reply_buf[offset] : -1;
}

void manifold_uart_clear(void)
{
    RING_SKIP(&manifold_uart_ring, RING_LEN(&manifold_uart_ring));
    manifold_reply_len = 0;
}

void manifold_set_term_out(void (*out_chars)(const char *buf, int len))
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

/* The UART is the only terminal here, and its sinks are the host wire, the
 * window terminal and the tx tap. Only a write to every terminal moves the
 * clean-line state. */
static void manifold_tx_write(uint8_t to, const char *buf, int len)
{
    if (to == COM_TERM_ALL)
        com_line_track(buf, (size_t)len);
    if (!(to & COM_TERM_UART))
        return;
    if (manifold_tx_tap)
        manifold_tx_tap(buf, len);
    if (com_get_bel())
        for (int i = 0; i < len; i++)
            if (buf[i] == '\a')
                bel_add(&bel_teletype);
    if (manifold_wire_tx)
        manifold_wire_tx(buf, len);
    if (manifold_term_out)
        manifold_term_out(buf, len);
}

static void manifold_crlf_write(const char *buf, int len)
{
    char out[2 * 64];
    int n = 0;
    for (int i = 0; i < len; i++)
    {
        char c = buf[i];
        if (c == '\n' && manifold_crlf_last != '\r')
            out[n++] = '\r';
        out[n++] = c;
        manifold_crlf_last = c;
        if (n >= (int)sizeof(out) - 1)
        {
            manifold_tx_write(COM_TERM_ALL, out, n);
            n = 0;
        }
    }
    if (n)
        manifold_tx_write(COM_TERM_ALL, out, n);
}

/* Output is written as it is made, so there is always room. */
bool manifold_cts(void)
{
    return true;
}

int manifold_putchar(int c)
{
    char ch = (char)c;
    manifold_crlf_write(&ch, 1);
    return (int)(unsigned char)c;
}

void manifold_putchar_raw(char c)
{
    if (manifold_std_tap)
        manifold_std_tap(STDOUT_FILENO, &c, 1);
    manifold_tx_write(COM_TERM_ALL, &c, 1);
}

void manifold_write_to(uint8_t to, const char *buf, size_t n)
{
    manifold_tx_write(to, buf, (int)n);
}

/* No telnet here, so the UART is the one terminal. */
uint8_t manifold_term_in_charge(void)
{
    return COM_TERM_UART;
}

uint8_t manifold_terms_connected(void)
{
    return COM_TERM_UART;
}

size_t manifold_write(int fd, const char *buf, size_t n)
{
    if (manifold_std_tap)
        manifold_std_tap(fd, buf, (int)n);
    if (fd == STDERR_FILENO && manifold_stderr_sink)
        manifold_stderr_sink(buf, (int)n);
    manifold_crlf_write(buf, (int)n);
    return n;
}

/* Output is written as it is made. */
void manifold_flush(void)
{
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

/* A Ctrl-C is scanned for before the bytes are pushed, so a break is seen
 * even when the ring is full and the byte itself is dropped. */
static void manifold_push(manifold_ring_t *r, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == MANIFOLD_ETX)
            ria_trigger_sigint();
    RING_IN(r, s, (uint32_t)n);
}

void manifold_keyboard_push(const char *s, size_t n)
{
    manifold_push(&manifold_keyboard_ring, s, n);
}

void manifold_keyboard_push_byte(uint8_t b)
{
    manifold_keyboard_push((const char *)&b, 1);
}

void manifold_uart_push(const char *s, size_t n)
{
    manifold_push(&manifold_uart_ring, s, n);
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
