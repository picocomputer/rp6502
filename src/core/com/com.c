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
#include "core/sys/com_term.h"
#include "core/com/tty.h"
#include "core/aud/bel.h"
#include "core/str/str.h"
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
/* The emulated terminal's answer, held whole until the wire it shares a
 * source with is empty. Promoted then and not before, so it always follows
 * everything the wire has delivered rather than jumping the queue into the
 * middle of it -- the same rule the VGA chip's console states for the same
 * reason. What no rule here can catch is a wire that stopped mid sequence
 * and has more coming; only the far end knows that. A reply is a bounded
 * burst, so one buffer holds any of them. */
static uint8_t reply_buf[32];
static size_t reply_len;
static bool com_term_reply_suppressed;

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
        return &uart_ring;
    /* No telnet on a machine of this shape. Answering the wire's ring here
     * would file its bytes under the wrong source in the line editor. */
    case COM_SOURCE_TEL:
        return NULL;
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

/* The wire is empty, so a held answer can go in without landing inside
 * something the wire had half-delivered. */
static void reply_promote(void)
{
    if (!reply_len || uart_ring.head != uart_ring.tail)
        return;
    for (size_t i = 0; i < reply_len; i++)
        ring_push(&uart_ring, reply_buf[i]);
    reply_len = 0;
}

/* The source a read is in the middle of. A merged read takes one source's
 * bytes until that source is dry, then moves on -- so a keystroke cannot cut
 * into a paste, and a wire delivering a file cannot starve the keyboard for
 * the two readers that take one byte at a time. Without it a raw TTY: read
 * could return an escape sequence from one source spliced with bytes from
 * another, in the same buffer.
 *
 * The contract has always promised this; only the machine with a real serial
 * port had it. */
static com_source_t rx_held = COM_SOURCE_ANY;

static int rx_pick(com_source_t *src)
{
    if (rx_held != COM_SOURCE_ANY)
    {
        ring_t *r = ring_for(rx_held);
        int c = r ? ring_pop(r) : -1;
        if (c >= 0)
        {
            *src = rx_held;
            return c;
        }
        rx_held = COM_SOURCE_ANY; /* dry: whoever speaks next holds it */
    }
    int c = ring_pop(&uart_ring);
    if (c >= 0)
    {
        *src = rx_held = COM_SOURCE_UART;
        return c;
    }
    c = ring_pop(&keyboard_ring);
    if (c >= 0)
    {
        *src = rx_held = COM_SOURCE_KEYBOARD;
        return c;
    }
    *src = COM_SOURCE_ANY;
    return -1;
}

int com_getchar(com_source_t *src)
{
    /* A byte the register window staged is older than anything in the rings,
     * and is stranded unless whoever reads next takes it back. */
    if (*src == COM_SOURCE_ANY || *src == COM_SOURCE_UART)
    {
        char staged;
        if (ria_rx_reclaim(&staged))
        {
            *src = COM_SOURCE_UART;
            return (unsigned char)staged;
        }
    }
    reply_promote();
    if (*src == COM_SOURCE_ANY)
        return rx_pick(src);
    ring_t *r = ring_for(*src);
    int c = r ? ring_pop(r) : -1;
    if (c < 0)
        *src = COM_SOURCE_ANY;
    return c;
}

int com_peekchar(com_source_t src)
{
    reply_promote();
    ring_t *r = ring_for(src);
    return r ? ring_peek(r) : -1;
}

size_t com_stdin_read(char *buf, size_t count)
{
    size_t n = 0;
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

/* The program's streams, raw, for whoever wants them apart from the terminal. */
static void (*com_std_tap)(int fd, const char *buf, int len);

void com_set_std_tap(void (*tap)(int fd, const char *buf, int len))
{
    com_std_tap = tap;
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

/* The wire takes the raw bytes; the terminal shows them beside stdout, so
 * nobody at the screen has an error hidden from them. */
size_t com_stderr_write(const char *buf, size_t count)
{
    if (com_std_tap)
        com_std_tap(2, buf, (int)count);
    tty_stderr_write(buf, (int)count);
    com_crlf_write(buf, (int)count);
    return count;
}

/* ---- input: what arrives, and what a Ctrl-C in it means ---- */

/* Dropped whole rather than truncated: half a CSI is a sequence the reader
 * would parse as something else. */
void com_in_write_reply(const char *s, size_t n)
{
    if (com_term_reply_suppressed || n > sizeof reply_buf - reply_len)
        return;
    for (size_t i = 0; i < n; i++)
        reply_buf[reply_len++] = (uint8_t)s[i];
}

void com_suppress_term_reply(bool suppress)
{
    com_term_reply_suppressed = suppress;
}

/* The wire's end of the console, shaped after a Pico draining its UART FIFO:
 * the SIGINT scan comes before the space check, so a Ctrl-C is caught even
 * when the byte after it is dropped. */
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

/* Cold boot: clear queued input and restore the BEL default. Not run per
 * program -- type-ahead survives an exec, and com_run resets the BEL alone. */
void com_init(void)
{
    memset(&keyboard_ring, 0, sizeof(keyboard_ring));
    memset(&uart_ring, 0, sizeof(uart_ring));
    reply_len = 0;
    rx_held = COM_SOURCE_ANY;
    com_bel_enabled = true;
}

void com_run(void)
{
    com_bel_enabled = true;
}

/* What was typed was meant for the program being interrupted. The register
 * window's staged byte goes with it, or a program that starts next reads a
 * character aimed at the one that just stopped. */
void com_break(void)
{
    char staged;
    ria_rx_reclaim(&staged);
    memset(&keyboard_ring, 0, sizeof(keyboard_ring));
    memset(&uart_ring, 0, sizeof(uart_ring));
    reply_len = 0;
    rx_held = COM_SOURCE_ANY;
}

void com_stop(void)
{
    /* The terminal is somebody's, and the guest may have left it in a mode
     * of its own. This is the same string the Pico's console signs off with.
     */
    com_printf("%s", STR_TERM_SOFT_RESET);
}
