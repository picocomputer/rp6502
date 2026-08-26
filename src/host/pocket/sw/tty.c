/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's end of the console wire: a word poked at the fabric, which
 * forwards it to the platform's bridge, and a copy to the log the savestate
 * carries. Plus the stream picolibc wants before printf will link.
 */

#include "core/com/tty.h"
#include "core/com.h"

#include "log.h"
#include "mmio.h"

#include <stdarg.h>
#include <stdio.h>

void tty_write(const char *buf, int len)
{
    for (int i = 0; i < len; i++)
    {
        MMIO_CONSOLE = (uint8_t)buf[i];
        log_putc(buf[i]);
    }
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

/* Streamed rather than buffered: the FILE below already reaches com_putchar,
 * and a 4 KB stack has no room for a formatting buffer. */
int com_printf(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = vprintf(fmt, va);
    va_end(va);
    return n;
}
