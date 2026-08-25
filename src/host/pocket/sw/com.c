/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pocket port of ria/sys/com.c. Input rings merge behind com_getchar
 * for the shared readline and stdin sources; CRLF expansion sits on the
 * console TX primitives. com_task drains the 6502's TX ring, answers the
 * $FFE2 offer slot, and polls the keyboard.
 */

#include "bel.h"
#include "com.h"
#include "log.h"
#include "mmio.h"
#include "core/aud/bel.h"
#include "core/hid/kbd.h"
#include "core/hid/kbt.h"

#include <stdio.h>
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

/* No register window to reclaim here: the fabric asks for a byte only
 * when the 6502 has one outstanding, so nothing is staged ahead of a
 * reader. See core/com.h, and ria/sys/com.c where a byte can be. */
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

int com_getchar(com_source_t *src)
{
    if (*src == COM_SOURCE_ANY)
    {
        /* Terminal replies before typed input: the CPR/DA handshake
         * resolves promptly and the keyboard never starves, because the
         * UART only fills in bounded reply bursts. */
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

static void (*com_term_out)(const char *buf, int len);

void com_set_term_out(void (*out_chars)(const char *buf, int len))
{
    com_term_out = out_chars;
}

static void com_tx_write(const char *buf, int len)
{
    for (int i = 0; i < len; i++)
    {
        MMIO_CONSOLE = (uint8_t)buf[i];
        log_putc(buf[i]);
        if (buf[i] == '\a' && com_bel_enabled)
            bel_add(&bel_teletype);
    }
    if (com_term_out)
        com_term_out(buf, len);
}

void com_in_write_reply(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ring_push(&uart_ring, (uint8_t)s[i]);
}

/* The pico-SDK translation the shared sources were written against. */
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

int com_printf(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = vprintf(fmt, va);
    va_end(va);
    return n;
}

/* picolibc wants a stream before printf will link. Pointing it at
 * com_putchar puts plain printf through the same CRLF expansion, BEL
 * scan and terminal tap as com_printf. */
static int com_stdio_putc(char c, FILE *f)
{
    (void)f;
    return com_putchar((unsigned char)c);
}

static FILE com_stdio = FDEV_SETUP_STREAM(com_stdio_putc, NULL, NULL,
                                          _FDEV_SETUP_WRITE);
FILE *const stdout = &com_stdio;
FILE *const stderr = &com_stdio;

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

// Cold boot only: type-ahead survives an exec, so com_run resets the BEL
// alone.
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
    /* Raw: the program speaks wire bytes, and a UART does not translate. */
    uint32_t v;
    while ((v = UART_POP) & 0x100)
    {
        char c = (char)v;
        com_tx_write(&c, 1);
    }

    /* Only on the ask: offering eagerly would commit bytes the console's
     * own readers still want, and an ask with nothing queued is answered
     * with nothing rather than remembered. Served before the keyboard
     * poll so a byte cannot arrive inside the same tick as an expired ask. */
    uint32_t st = RX_OFFER;
    if ((st & 3) == 3)
    {
        com_source_t src = COM_SOURCE_ANY;
        int c = com_getchar(&src);
        if (c >= 0)
        {
            RX_OFFER = (uint32_t)c;
        }
        else
        {
            RX_OFFER = 0x200;
        }
    }

    /* Bit 8 is the valid flag. Testbench only; nothing on hardware
     * drives this register. */
    uint32_t k = MMIO_KBD;
    if (k & 0x100)
    {
        ring_push(&kbd_ring, (uint8_t)k);
    }

    char buf[16];
    size_t n = kbt_in_chars(buf, sizeof buf);
    for (size_t i = 0; i < n; i++)
        ring_push(&kbd_ring, (uint8_t)buf[i]);
}
