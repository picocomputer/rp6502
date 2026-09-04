/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console, for a machine whose console has no wire of its own to
 * arbitrate: two rings, one for what was typed and one for what the terminal
 * answered, and a single sink every terminal-bound byte passes through once.
 *
 * The rules here are the machine's, not the wire's -- a Ctrl-C latches a
 * SIGINT wherever it enters, a BEL in program output rings the teletype, a
 * bare newline is spelled CRLF -- so they are written once and every machine
 * of this shape gets all of them. The wire is core/com/tty.h.
 */

#include "core/sys/ria.h"
#include "core/com/com.h"
#include "core/com/tty.h"
#include "core/aud/bel.h"
#include "core/sys/driver.h"

#include <stdio.h>
#include <string.h>

/* A Ctrl-C anywhere in the keyboard stream latches a SIGINT. The byte is still
 * delivered to the program; the latch is independent of consumption, and it
 * happens here so no machine can be the one that forgets to do it. */
#define COM_ETX 0x03

#define RING_MASK (COM_RING_SIZE - 1)
_Static_assert((COM_RING_SIZE & RING_MASK) == 0, "COM_RING_SIZE must be a power of two");

typedef struct
{
    uint8_t buf[COM_RING_SIZE];
    uint16_t head; /* next write */
    uint16_t tail; /* next read */
} ring_t;

static ring_t keyboard_ring;
static ring_t uart_ring;

/* Gates the teletype bell rung on a BEL (0x07) in program output. The setting
 * roundtrips through the BEL attribute, so a program reads back what it set. */
static bool com_bel_enabled = true;

static ring_t *ring_for(com_source_t src)
{
    switch (src)
    {
    case COM_SOURCE_KEYBOARD:
        return &keyboard_ring;
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
        /* Terminal replies before typed input, so the CPR/DA handshake
         * resolves promptly; the keyboard never starves, because the reply
         * ring only fills in bounded bursts. */
        int c = ring_pop(&uart_ring);
        if (c >= 0)
        {
            *src = COM_SOURCE_UART;
            return c;
        }
        c = ring_pop(&keyboard_ring);
        if (c >= 0)
        {
            *src = COM_SOURCE_KEYBOARD;
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

size_t com_stdin_read(char *buf, size_t count)
{
    size_t n = 0;
    /* A machine that stages a byte in its register window to answer a ready
     * bit has to take it back, or a program polling the one while reading the
     * console through the other leaves it stranded. */
    if (n < count && tty_reg_reclaim(&buf[n]))
        n++;
    for (; n < count; n++)
    {
        com_source_t src = COM_SOURCE_ANY;
        int c = com_getchar(&src);
        if (c < 0)
            break;
        buf[n] = (char)c;
    }
    return n;
}

/* ---- output: the one path to the terminal ---- */

static void (*com_term_out)(const char *buf, int len);

void com_set_term_out(void (*out_chars)(const char *buf, int len))
{
    com_term_out = out_chars;
}

/* Optional tap on the terminal stream, set by tests to capture output. */
static void (*com_tx_tap)(const char *buf, int len);

void com_set_tx_tap(void (*tap)(const char *buf, int len))
{
    com_tx_tap = tap;
}

/* Every terminal-bound byte passes here exactly once, after CRLF translation:
 * the tap, the bell and the wire all observe the same merged stream. */
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

/* The shared sources were written against a stdio layer that translated above
 * the driver, so a bare '\n' reaches the terminal as "\r\n". Batched, because
 * the sink is a call and one per byte is a call per byte.
 *
 * Public because com_printf is each machine's -- how it formats is its libc's
 * business, and a soft CPU with a 4 KB stack does not want the buffer a
 * vsnprintf form needs -- but every machine's printf ends here. */
void com_crlf_write(const char *buf, int len)
{
    static char last;
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


/* The terminal sink never backpressures on these machines: a write is always
 * ready and completes on the spot. */

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
    com_tx_write(&ch, 1);
}

/* ---- input: what arrives, and what a Ctrl-C in it means ---- */

void com_in_write_reply(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ring_push(&uart_ring, (uint8_t)s[i]);
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
    return (size_t)((keyboard_ring.tail - keyboard_ring.head - 1) & RING_MASK);
}

bool com_get_bel(void)
{
    return com_bel_enabled;
}

void com_set_bel(bool value)
{
    com_bel_enabled = value;
}

/* Cold boot: clear queued input and restore the BEL default. Not run per
 * program -- type-ahead survives an exec, and com_run resets the BEL alone. */
void com_init(void)
{
    memset(&keyboard_ring, 0, sizeof(keyboard_ring));
    memset(&uart_ring, 0, sizeof(uart_ring));
    com_bel_enabled = true;
}

void com_run(void)
{
    com_bel_enabled = true;
}
