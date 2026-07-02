/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console (firmware analog: ria/sys/com.c), both directions.
 *
 * Input rings feeding the vendored line editor (rln.c). The firmware reads
 * keyboard/UART/telnet bytes through com; the emulator needs two streams:
 *
 *   KBD  - user keystrokes from the kbd.c replacement (sokol events).
 *   UART - the terminal's in-band replies (term.c -> com_in_write_reply),
 *          which carry the CPR/DA answers rln's geometry handshake expects.
 *
 * rln tags CPR/DA tracking by source, so the two streams stay separate and
 * are returned with their true COM_SOURCE_* identity.
 *
 * Output: bytes from the stdout/console write syscall (and the reused firmware
 * putchar/printf) feed the vendored terminal via its captured stdio driver,
 * applying the firmware's CRLF translation.
 */

#include "sys/com.h"
#include "sys/ria.h"
#include "aud/bel.h"
#include "pico/stdio/driver.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A Ctrl-C (0x03) anywhere in the keyboard stream latches a SIGINT, exactly
 * like the firmware's com/kbd scan (ria/sys/com.c, ria/hid/kbd.c). The byte is
 * still delivered to the program; the latch is independent of consumption. */
#define COM_ETX 0x03

#define RING_SIZE 512 /* power of two */
#define RING_MASK (RING_SIZE - 1)

typedef struct
{
    uint8_t buf[RING_SIZE];
    uint16_t head; /* next write */
    uint16_t tail; /* next read */
} ring_t;

static ring_t kbd_ring;
static ring_t uart_ring;

/* The bell-enable flag (firmware: com.c). Gates the teletype bell rung on a BEL
 * (0x07) in program console output; the setting also roundtrips through the BEL
 * attribute so a program reads back what it set. Defaults on, like the firmware. */
static bool com_bel_enabled = true;

static ring_t *ring_for(com_source_t src)
{
    switch (src)
    {
    case COM_SOURCE_KBD:
        return &kbd_ring;
    case COM_SOURCE_UART:
    case COM_SOURCE_TEL:
        return &uart_ring;
    default:
        return NULL;
    }
}

static void ring_push(ring_t *r, uint8_t b)
{
    uint16_t next = (uint16_t)((r->head + 1) & RING_MASK);
    if (next == r->tail)
        return; /* full: drop */
    r->buf[r->head] = b;
    r->head = next;
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

int com_getchar(com_source_t *src)
{
    if (*src == COM_SOURCE_ANY)
    {
        /* Drain terminal replies before typed input so the CPR/DA handshake
         * resolves promptly; keyboard input is queued and never starves
         * (UART only fills in bounded reply bursts). */
        int c = ring_pop(&uart_ring);
        if (c >= 0)
        {
            *src = COM_SOURCE_UART;
            return c;
        }
        c = ring_pop(&kbd_ring);
        if (c >= 0)
        {
            *src = COM_SOURCE_KBD;
            return c;
        }
        *src = COM_SOURCE_ANY;
        return -1;
    }
    ring_t *r = ring_for(*src);
    int c = r ? ring_pop(r) : -1;
    if (c < 0)
        *src = COM_SOURCE_ANY;
    return c;
}

int com_peekchar(com_source_t src)
{
    ring_t *r = ring_for(src);
    return r ? ring_peek(r) : -1;
}

/* ------------------------------------------------------------------ */
/* Console output: the terminal's captured stdio driver                 */
/* ------------------------------------------------------------------ */

static stdio_driver_t *g_stdio;

void stdio_set_driver_enabled(stdio_driver_t *driver, bool enabled)
{
    g_stdio = enabled ? driver : NULL;
}

/* Optional tap on the raw terminal stream (set by tests to capture output). */
static void (*g_stdout_tap)(const char *buf, int len);

void com_set_stdout_tap(void (*tap)(const char *buf, int len))
{
    g_stdout_tap = tap;
}

/* Echo/tap/bell processing common to the translated and raw sinks. The bell
 * rings on any BEL in the terminal stream, like the firmware's TX-drain scan
 * (ria/sys/com.c). */
static void stdout_taps(const char *buf, int len)
{
    /* Bring-up aid: EMU_ECHO mirrors the terminal stream to the host's stderr
     * so the program's text output is visible without rendering the frame. */
    static int echo = -1;
    if (echo < 0)
        echo = getenv("EMU_ECHO") ? 1 : 0;
    if (echo)
        fwrite(buf, 1, (size_t)len, stderr);
    if (g_stdout_tap)
        g_stdout_tap(buf, len);
    if (com_get_bel())
        for (int i = 0; i < len; i++)
            if (buf[i] == '\a')
                bel_add(&bel_teletype);
}

void com_stdout_write(const char *buf, int len)
{
    static char last;
    stdout_taps(buf, len);
    if (!g_stdio || !g_stdio->out_chars)
        return;
    /* Replicate pico stdio CRLF translation: a bare '\n' becomes "\r\n". */
    if (!g_stdio->crlf_enabled)
    {
        g_stdio->out_chars(buf, len);
        return;
    }
    char out[2 * 64];
    int n = 0;
    for (int i = 0; i < len; i++)
    {
        char c = buf[i];
        if (c == '\n' && last != '\r')
            out[n++] = '\r';
        out[n++] = c;
        last = c;
        if (n >= (int)sizeof(out) - 1)
        {
            g_stdio->out_chars(out, n);
            n = 0;
        }
    }
    if (n)
        g_stdio->out_chars(out, n);
}

void com_stdout_write_raw(const char *buf, int len)
{
    stdout_taps(buf, len);
    if (g_stdio && g_stdio->out_chars)
        g_stdio->out_chars(buf, len);
}

/* The reused firmware sources (rln.c) echo input and emit ANSI handshakes via
 * putchar/printf, redirected here by the pico/stdlib.h shim so they reach the
 * same terminal sink as the 6502's stdout syscall. */

int com_term_putchar(int c)
{
    char ch = (char)c;
    com_stdout_write(&ch, 1);
    return (int)(unsigned char)c;
}

int com_term_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return n;
    int w = (n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1;
    com_stdout_write(buf, w);
    return n;
}

/* Output side of the shared contract. The terminal sink never backpressures,
 * so writes are always ready and complete instantly. */

bool com_putchar_ready(void)
{
    return true;
}

bool com_writable(void)
{
    return true;
}

void com_write(char ch)
{
    com_stdout_write_raw(&ch, 1);
}

void com_in_write_reply(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ring_push(&uart_ring, (uint8_t)s[i]);
}

void com_kbd_push(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if ((uint8_t)s[i] == COM_ETX)
            ria_trigger_sigint();
        ring_push(&kbd_ring, (uint8_t)s[i]);
    }
}

void com_kbd_push_byte(uint8_t b)
{
    if (b == COM_ETX)
        ria_trigger_sigint();
    ring_push(&kbd_ring, b);
}

bool com_get_bel(void)
{
    return com_bel_enabled;
}

void com_set_bel(bool value)
{
    com_bel_enabled = value;
}

void com_reset(void)
{
    memset(&kbd_ring, 0, sizeof(kbd_ring));
    memset(&uart_ring, 0, sizeof(uart_ring));
    com_bel_enabled = true;
}
