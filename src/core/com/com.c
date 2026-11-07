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
#include "core/sys/ring.h"
#include "machine.h"

#include <stdio.h>
#include <string.h>

#define COM_ETX 0x03

typedef RING(uint8_t, COM_RING_SIZE) com_ring_t;

static com_ring_t com_keyboard_ring;
static com_ring_t com_uart_ring;
static uint8_t com_reply_buf[32];
static size_t com_reply_len;
static bool com_term_reply_suppressed;

static bool com_bel_enabled = true;

static char com_crlf_last;

/* The count, then COM_RING_SIZE bytes oldest first. */
static void com_ring_save(sst_cursor_t *c, com_ring_t *r)
{
    const uint32_t len = RING_LEN(r);
    sst_put_u16(c, (uint16_t)len);
    for (uint32_t i = 0; i < COM_RING_SIZE; i++)
        sst_put_u8(c, i < len ? *RING_AT(r, i) : 0);
}

static uint16_t com_ring_get(sst_cursor_t *c, uint8_t *buf)
{
    const uint16_t len = sst_get_u16(c);
    sst_get(c, buf, COM_RING_SIZE);
    return len;
}

static void com_ring_set(com_ring_t *r, const uint8_t *buf, uint16_t len)
{
    RING_RESET(r);
    RING_IN(r, buf, len);
}

void com_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    com_ring_save(c, &com_keyboard_ring);
    com_ring_save(c, &com_uart_ring);
    sst_put_u8(c, (uint8_t)com_reply_len);
    sst_put(c, com_reply_buf, sizeof com_reply_buf);
    sst_put_bool(c, com_bel_enabled);
    sst_put_u8(c, (uint8_t)com_crlf_last);
    com_rx_save(c);
}

bool com_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint8_t kb[COM_RING_SIZE], wire[COM_RING_SIZE];
    const uint16_t kb_len = com_ring_get(c, kb);
    const uint16_t wire_len = com_ring_get(c, wire);
    uint8_t len = sst_get_u8(c);
    uint8_t held[sizeof com_reply_buf];
    sst_get(c, held, sizeof held);
    bool bel = sst_get_bool(c);
    uint8_t crlf = sst_get_u8(c);
    if (!sst_ok(c) || kb_len > COM_RING_SIZE || wire_len > COM_RING_SIZE ||
        len > sizeof com_reply_buf)
        return false;
    if (!com_rx_load(c))
        return false;
    com_ring_set(&com_keyboard_ring, kb, kb_len);
    com_ring_set(&com_uart_ring, wire, wire_len);
    com_reply_len = len;
    memcpy(com_reply_buf, held, sizeof com_reply_buf);
    com_bel_enabled = bel;
    com_crlf_last = (char)crlf;
    return true;
}

/* A reply is held back while the ring still has bytes in it, because those
 * bytes can be the start of a sequence whose rest has not arrived yet, and
 * the reply would then be delivered in the middle of that sequence. */
static void com_reply_promote(void)
{
    if (!com_reply_len || RING_LEN(&com_uart_ring))
        return;
    RING_IN(&com_uart_ring, com_reply_buf, (uint32_t)com_reply_len);
    com_reply_len = 0;
}

size_t com_keyboard_read(char *buf, size_t length)
{
    return RING_OUT(&com_keyboard_ring, buf, (uint32_t)length);
}

void com_keyboard_clear(void)
{
    RING_SKIP(&com_keyboard_ring, RING_LEN(&com_keyboard_ring));
}

size_t com_uart_read(char *buf, size_t length)
{
    size_t n = 0;
    while (n < length)
    {
        com_reply_promote();
        const uint32_t got = RING_OUT(&com_uart_ring, buf + n, (uint32_t)(length - n));
        if (!got)
            break;
        n += got;
    }
    return n;
}

int com_uart_peek(void)
{
    com_reply_promote();
    return RING_LEN(&com_uart_ring) ? *RING_AT(&com_uart_ring, 0) : -1;
}

void com_uart_clear(void)
{
    RING_SKIP(&com_uart_ring, RING_LEN(&com_uart_ring));
    com_reply_len = 0;
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
    if (com_term_reply_suppressed || n > sizeof com_reply_buf ||
        com_reply_len > sizeof com_reply_buf - n)
        return;
    memcpy(com_reply_buf + com_reply_len, s, n);
    com_reply_len += n;
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
        if (s[i] == COM_ETX)
            ria_trigger_sigint();
    RING_IN(&com_uart_ring, s, (uint32_t)n);
}

void com_stream_push(const char *s, size_t n)
{
    RING_IN(&com_uart_ring, s, (uint32_t)n);
}

size_t com_uart_free(void)
{
    return RING_AVAIL(&com_uart_ring);
}

bool com_input_idle(void)
{
    return !com_reply_len && !RING_LEN(&com_uart_ring) &&
           !RING_LEN(&com_keyboard_ring);
}

void com_keyboard_push(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == COM_ETX)
            ria_trigger_sigint();
    RING_IN(&com_keyboard_ring, s, (uint32_t)n);
}

void com_keyboard_push_byte(uint8_t b)
{
    if (b == COM_ETX)
        ria_trigger_sigint();
    RING_PUT(&com_keyboard_ring, b);
}

size_t com_keyboard_free(void)
{
    return RING_AVAIL(&com_keyboard_ring);
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
