/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * tty_write is empty because console output on this machine goes only to the
 * screen, and com_tx_write already sends it there through com_term_out.
 * MMIO_CONSOLE is the port behind the debug pin and the Pocket's debug log,
 * where its bytes arrive in target command 0x0152 events, and nothing a
 * program prints is written to it.
 */

#include "core/com/tty.h"
#include "core/sys/com.h"
#include "core/sys/debug_log.h"

#include "mmio.h"

#include <stdarg.h>
#include <stdio.h>

void tty_write(const char *buf, int len)
{
    (void)buf;
    (void)len;
}

static int tty_stdio_putc(char c, FILE *f)
{
    (void)f;
    return com_putchar((unsigned char)c);
}

static FILE tty_stdio = FDEV_SETUP_STREAM(tty_stdio_putc, NULL, NULL,
                                          _FDEV_SETUP_WRITE);
FILE *const stdout = &tty_stdio;
FILE *const stderr = &tty_stdio;

/* com_printf formats straight to stdout instead of into a 1024-byte buffer
 * as core/com/tty.c does, because the stack is only 4 KB. */
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
