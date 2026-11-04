/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/sys/ria.h"
#include "core/com/com.h"
#include "core/sys/com_term.h"
#include "core/com/tty.h"
#include "core/aud/bel.h"
#include "core/str/str.h"
#include "core/sys/driver.h"
#include "machine.h"

#include <stdio.h>
#include <string.h>

#define COM_ETX 0x03

#define RING_MASK (COM_RING_SIZE - 1)
_Static_assert((COM_RING_SIZE & RING_MASK) == 0, "COM_RING_SIZE must be a power of two");

typedef struct
{
    uint8_t buf[COM_RING_SIZE];
    uint16_t head;
    uint16_t tail;
} ring_t;

static ring_t keyboard_ring;
static ring_t uart_ring;
static uint8_t reply_buf[32];
static size_t reply_len;
static bool com_term_reply_suppressed;

static bool com_bel_enabled = true;

static char com_crlf_last;

static void ring_save(sst_cursor_t *c, const ring_t *r)
{
    sst_put(c, r->buf, COM_RING_SIZE);
    sst_put_u16(c, r->head);
    sst_put_u16(c, r->tail);
}

static bool ring_load(sst_cursor_t *c, ring_t *r)
{
    uint8_t buf[COM_RING_SIZE];
    sst_get(c, buf, sizeof buf);
    uint16_t head = sst_get_u16(c), tail = sst_get_u16(c);
    if (!sst_ok(c) || head >= COM_RING_SIZE || tail >= COM_RING_SIZE)
        return false;
    memcpy(r->buf, buf, sizeof buf);
    r->head = head;
    r->tail = tail;
    return true;
}

void com_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    ring_save(c, &keyboard_ring);
    ring_save(c, &uart_ring);
    sst_put_u8(c, (uint8_t)reply_len);
    sst_put(c, reply_buf, sizeof reply_buf);
    sst_put_bool(c, com_bel_enabled);
    sst_put_u8(c, (uint8_t)com_crlf_last);
    com_rx_save(c);
}

bool com_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    ring_t kb, wire;
    if (!ring_load(c, &kb) || !ring_load(c, &wire))
        return false;
    uint8_t len = sst_get_u8(c);
    uint8_t held[sizeof reply_buf];
    sst_get(c, held, sizeof held);
    bool bel = sst_get_bool(c);
    uint8_t crlf = sst_get_u8(c);
    if (!sst_ok(c) || len > sizeof reply_buf)
        return false;
    if (!com_rx_load(c))
        return false;
    keyboard_ring = kb;
    uart_ring = wire;
    reply_len = len;
    memcpy(reply_buf, held, sizeof reply_buf);
    com_bel_enabled = bel;
    com_crlf_last = (char)crlf;
    return true;
}

static void ring_push(ring_t *r, uint8_t b)
{
    uint16_t next = (uint16_t)((r->head + 1) & RING_MASK);
    if (next == r->tail)
        return;
    r->buf[r->head] = b;
    r->head = next;
}

static size_t ring_free(const ring_t *r)
{
    return (size_t)((r->tail - r->head - 1) & RING_MASK);
}

static int ring_peek(const ring_t *r)
{
    if (r->head == r->tail)
        return -1;
    return r->buf[r->tail];
}

static int ring_pop(ring_t *r)
{
    if (r->head == r->tail)
        return -1;
    uint8_t b = r->buf[r->tail];
    r->tail = (uint16_t)((r->tail + 1) & RING_MASK);
    return b;
}

/* A reply is held back while the ring still has bytes in it, because those
 * bytes can be the start of a sequence whose rest has not arrived yet, and
 * the reply would then be delivered in the middle of that sequence. */
static void reply_promote(void)
{
    if (!reply_len || uart_ring.head != uart_ring.tail)
        return;
    for (size_t i = 0; i < reply_len; i++)
        ring_push(&uart_ring, reply_buf[i]);
    reply_len = 0;
}

size_t com_keyboard_read(char *buf, size_t length)
{
    size_t n = 0;
    for (; n < length; n++)
    {
        int c = ring_pop(&keyboard_ring);
        if (c < 0)
            break;
        buf[n] = (char)c;
    }
    return n;
}

void com_keyboard_clear(void)
{
    memset(&keyboard_ring, 0, sizeof keyboard_ring);
}

size_t com_uart_read(char *buf, size_t length)
{
    size_t n = 0;
    for (; n < length; n++)
    {
        reply_promote();
        int c = ring_pop(&uart_ring);
        if (c < 0)
            break;
        buf[n] = (char)c;
    }
    return n;
}

int com_uart_peek(void)
{
    reply_promote();
    return ring_peek(&uart_ring);
}

void com_uart_clear(void)
{
    memset(&uart_ring, 0, sizeof uart_ring);
    reply_len = 0;
}

static void (*com_term_out)(const char *buf, int len);

void com_set_term_out(void (*out_chars)(const char *buf, int len))
{
    com_term_out = out_chars;
}

static void (*com_tx_tap)(const char *buf, int len);

void com_set_tx_tap(void (*tap)(const char *buf, int len))
{
    com_tx_tap = tap;
}

static void (*com_std_tap)(int fd, const char *buf, int len);

void com_set_std_tap(void (*tap)(int fd, const char *buf, int len))
{
    com_std_tap = tap;
}

static void (*com_stderr_sink)(const char *buf, int len);

void com_set_stderr_sink(void (*sink)(const char *buf, int len))
{
    com_stderr_sink = sink;
}

void com_tx_write(const char *buf, int len)
{
    if (com_tx_tap)
        com_tx_tap(buf, len);
    if (com_bel_enabled)
        for (int i = 0; i < len; i++)
            if (buf[i] == '\a')
                bel_add(&bel_teletype);
    tty_write(buf, len);
    if (com_term_out)
        com_term_out(buf, len);
}

void com_crlf_write(const char *buf, int len)
{
    char out[2 * 64];
    int n = 0;
    for (int i = 0; i < len; i++)
    {
        char c = buf[i];
        if (c == '\n' && com_crlf_last != '\r')
            out[n++] = '\r';
        out[n++] = c;
        com_crlf_last = c;
        if (n >= (int)sizeof(out) - 1)
        {
            com_tx_write(out, n);
            n = 0;
        }
    }
    if (n)
        com_tx_write(out, n);
}

int com_putchar(int c)
{
    char ch = (char)c;
    com_crlf_write(&ch, 1);
    return (int)(unsigned char)c;
}

bool com_writable(void)
{
    return true;
}

void com_write(char ch)
{
    if (com_std_tap)
        com_std_tap(1, &ch, 1);
    com_tx_write(&ch, 1);
}

size_t com_stdout_write(const char *buf, size_t count)
{
    if (com_std_tap)
        com_std_tap(1, buf, (int)count);
    com_crlf_write(buf, (int)count);
    return count;
}

size_t com_stderr_write(const char *buf, size_t count)
{
    if (com_std_tap)
        com_std_tap(2, buf, (int)count);
    if (com_stderr_sink)
        com_stderr_sink(buf, (int)count);
    com_crlf_write(buf, (int)count);
    return count;
}

/* A reply that does not fit is dropped whole rather than truncated, because
 * half of an escape sequence is a sequence the reader parses as something
 * else. */
void com_in_write_reply(const char *s, size_t n)
{
    if (com_term_reply_suppressed || n > sizeof reply_buf ||
        reply_len > sizeof reply_buf - n)
        return;
    memcpy(reply_buf + reply_len, s, n);
    reply_len += n;
}

void com_suppress_term_reply(bool suppress)
{
    com_term_reply_suppressed = suppress;
}

/* A Ctrl-C is scanned for before the byte is pushed, so a break is seen even
 * when the ring is full and the byte itself is dropped. */
void com_uart_push(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint8_t b = (uint8_t)s[i];
        if (b == COM_ETX)
            ria_trigger_sigint();
        ring_push(&uart_ring, b);
    }
}

void com_stream_push(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ring_push(&uart_ring, (uint8_t)s[i]);
}

size_t com_uart_free(void)
{
    return ring_free(&uart_ring);
}

bool com_input_idle(void)
{
    return !reply_len && uart_ring.head == uart_ring.tail &&
           keyboard_ring.head == keyboard_ring.tail;
}

void com_keyboard_push(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        com_keyboard_push_byte((uint8_t)s[i]);
}

void com_keyboard_push_byte(uint8_t b)
{
    if (b == COM_ETX)
        ria_trigger_sigint();
    ring_push(&keyboard_ring, b);
}

size_t com_keyboard_free(void)
{
    return ring_free(&keyboard_ring);
}

bool com_get_bel(void)
{
    return com_bel_enabled;
}

void com_set_bel(bool value)
{
    com_bel_enabled = value;
}

void com_init(void)
{
    com_rx_clear();
    com_bel_enabled = true;
}

void com_run(void)
{
    com_bel_enabled = true;
}

void com_break(void)
{
    com_rx_clear();
}

void com_stop(void)
{
    com_printf("%s", STR_TERM_SOFT_RESET);
}
