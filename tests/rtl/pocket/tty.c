/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The bench's end of the Pocket's console wire: a program's console and the
 * machine's own lines both go to the port the bench reads, one word poked at
 * the fabric per byte. Plus the stream picolibc wants before printf will link.
 */

#include "core/com/tty.h"
#include "core/sys/com.h"
#include "core/sys/debug_log.h"

#include "host/pocket/sw/mmio.h"

#include <stdarg.h>
#include <stdio.h>

void tty_write(const char *buf, int len)
{
    for (int i = 0; i < len; i++)
    {
        MMIO_CONSOLE = (uint8_t)buf[i];
    }
}

void tty_stderr_write(const char *buf, int len)
{
    (void)buf;
    (void)len;
}

/* The fabric asks for a byte only when the 6502 has one outstanding, so
 * nothing is ever staged ahead of a reader. */
bool tty_reg_reclaim(char *out)
{
    (void)out;
    return false;
}

/* picolibc wants a stream before printf will link. Pointing it at com_putchar
 * puts a plain printf through the same CRLF expansion, bell scan and terminal
 * tap as com_printf. */
static int tty_stdio_putc(char c, FILE *f)
{
    (void)f;
    return com_putchar((unsigned char)c);
}

static FILE tty_stdio = FDEV_SETUP_STREAM(tty_stdio_putc, NULL, NULL,
                                          _FDEV_SETUP_WRITE);
FILE *const stdout = &tty_stdio;
FILE *const stderr = &tty_stdio;

/* Streamed rather than buffered: the FILE above already reaches com_putchar,
 * and a 4 KB stack has no room for a formatting buffer. */
int com_printf(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = vprintf(fmt, va);
    va_end(va);
    return n;
}

static int port_putc(char c, FILE *f)
{
    (void)f;
    MMIO_CONSOLE = (uint8_t)c;
    return c;
}

static FILE port = FDEV_SETUP_STREAM(port_putc, NULL, NULL, _FDEV_SETUP_WRITE);

void host_log(int level, const char *category, const char *fmt, ...)
{
    static const char *const names[] = RP6502_LOG_LEVEL_NAMES;
    fprintf(&port, "%s %s: ", names[level], category);
    va_list va;
    va_start(va, fmt);
    vfprintf(&port, fmt, va);
    va_end(va);
    fputc('\n', &port);
}
