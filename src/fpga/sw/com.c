/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pocket port of ria/sys/com.c, shaped like the emulator's proven trim:
 * input rings merged behind com_getchar for the shared readline and stdin
 * sources, CRLF expansion on the console TX primitives. The manifold's
 * wires are this machine's: com_task drains the 6502's hardware TX ring to
 * the console, polls the platform keyboard, and keeps the $FFE2 offer slot
 * topped up — the greedy latch refill emu/sys/ria.c does on demand.
 *
 * The console sink is the simulation console until the terminal arrives;
 * the BEL flag is kept for the shared setters, the scan waits for audio.
 */

#include "com.h"
#include "mmio.h"

#include <string.h>

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

static bool com_bel_enabled = true;
static uint32_t com_moved_count;

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
        /* Terminal replies before typed input, like the emulator: the
         * CPR/DA handshake resolves promptly and the keyboard never
         * starves (UART only fills in bounded reply bursts). */
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

/* The single console sink: every terminal-bound byte passes here exactly
 * once, after any CRLF translation, where the BEL scan will live. */
static void com_tx_write(const char *buf, int len)
{
    for (int i = 0; i < len; i++)
        MMIO_CONSOLE = (uint8_t)buf[i];
    com_moved_count += (uint32_t)len;
}

/* CRLF expansion above the sink, the pico-SDK translation the shared
 * sources were written against: a bare '\n' becomes "\r\n". */
static void com_crlf_write(const char *buf, int len)
{
    static char last;
    for (int i = 0; i < len; i++)
    {
        char c = buf[i];
        if (c == '\n' && last != '\r')
            com_tx_write("\r", 1);
        com_tx_write(&c, 1);
        last = c;
    }
}

int com_putchar(int c)
{
    char ch = (char)c;
    com_crlf_write(&ch, 1);
    return (int)(unsigned char)c;
}

/* The console never backpressures; the simulation sink is always ready. */

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

bool com_get_bel(void)
{
    return com_bel_enabled;
}

void com_set_bel(bool value)
{
    com_bel_enabled = value;
}

uint32_t com_moved(void)
{
    return com_moved_count;
}

// Cold-boot flush: clear queued input and reset the BEL default. NOT run
// per program — type-ahead survives an exec; com_run resets the BEL alone.
void com_init(void)
{
    memset(&kbd_ring, 0, sizeof(kbd_ring));
    memset(&uart_ring, 0, sizeof(uart_ring));
    com_bel_enabled = true;
}

void com_run(void)
{
    com_bel_enabled = true;
}

void com_task(void)
{
    /* The 6502's TX wire goes to the console raw; the program already
     * speaks wire bytes, no translation on a UART. Drained dry, like the
     * firmware's own TX task. */
    uint32_t v;
    while ((v = UART_POP) & 0x100)
    {
        char c = (char)v;
        com_tx_write(&c, 1);
    }

    /* The platform keyboard feeds the KBD ring. */
    uint32_t k = MMIO_KBD;
    if (k & 0x100)
    {
        ring_push(&kbd_ring, (uint8_t)k);
        com_moved_count++;
    }

    /* Keep the $FFE2 offer slot topped up from the merged sources. */
    if (RX_OFFER & 1)
    {
        com_source_t src = COM_SOURCE_ANY;
        int c = com_getchar(&src);
        if (c >= 0)
        {
            RX_OFFER = (uint32_t)c;
            com_moved_count++;
        }
    }
}
