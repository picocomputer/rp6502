/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "ria/api/oem.h"
#include "emu/sys/com.h"
#include "emu/sys/ria.h"
#include "ria/aud/bel.h"
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
/* Console output: the TX wire to the terminal                          */
/* ------------------------------------------------------------------ */

/* The wire to the VGA terminal (the captured stdio driver's out_chars) — the
 * analog of the firmware's UART/PIX fanout target. The pico stdio shim
 * registers it when term.c enables its driver. */
static void (*com_term_out)(const char *buf, int len);

void com_set_term_out(void (*out_chars)(const char *buf, int len))
{
    com_term_out = out_chars;
}

/* Optional tap on the terminal OUT stream (set by tests to capture output). */
static void (*com_tx_tap)(const char *buf, int len);

void com_set_tx_tap(void (*tap)(const char *buf, int len))
{
    com_tx_tap = tap;
}

/* The single terminal sink — the analog of the firmware's UART TX drain
 * (com_uart_drain_tx): every terminal-bound byte passes here exactly once,
 * after any CRLF translation. The tap, the EMU_ECHO mirror, and the BEL scan
 * observe the merged stream at this point, like the firmware's drain. */
void com_tx_write(const char *buf, int len)
{
    /* Bring-up aid: EMU_ECHO mirrors the terminal stream to the host's stderr
     * so the program's text output is visible without rendering the frame.
     * Host streams carry host encoding, so OEM bytes expand to UTF-8. */
    static int echo = -1;
    if (echo < 0)
        echo = getenv("EMU_ECHO") ? 1 : 0;
    if (echo)
        for (int i = 0; i < len; i++)
        {
            char enc[3];
            fwrite(enc, 1, (size_t)oem_to_utf8_char((unsigned char)buf[i], enc), stderr);
        }
    if (com_tx_tap)
        com_tx_tap(buf, len);
    if (com_get_bel())
        for (int i = 0; i < len; i++)
            if (buf[i] == '\a')
                bel_add(&bel_teletype);
    if (com_term_out)
        com_term_out(buf, len);
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
    com_tx_write(&ch, 1);
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

size_t com_kbd_free(void)
{
    return (size_t)((kbd_ring.tail - kbd_ring.head - 1) & RING_MASK);
}

bool com_get_bel(void)
{
    return com_bel_enabled;
}

void com_set_bel(bool value)
{
    com_bel_enabled = value;
}

// Cold-boot flush: clear queued input and reset the BEL default. NOT run per
// program — type-ahead survives an exec; com_run resets the BEL alone.
void com_init(void)
{
    memset(&kbd_ring, 0, sizeof(kbd_ring));
    memset(&uart_ring, 0, sizeof(uart_ring));
    com_bel_enabled = true;
}

// Program start: restore the BEL default; the queued input rings are kept so
// type-ahead survives an exec (firmware com_run).
void com_run(void)
{
    com_bel_enabled = true;
}
