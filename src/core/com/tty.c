/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's end of the console wire: where a host that has one installs
 * it, and the pump that fills the console from it. A machine whose console is
 * the terminal it already renders installs nothing and this does nothing.
 */

#include "core/com/tty.h"
#include "core/com/com.h"

#include <stdarg.h>
#include <stdio.h>

/* The host's end of the wire, when the host has one. */
static void (*tty_tx)(const char *buf, int len);
static size_t (*tty_rx)(char *buf, size_t max);

void tty_set_wire(void (*tx)(const char *buf, int len),
                  size_t (*rx)(char *buf, size_t max))
{
    tty_tx = tx;
    tty_rx = rx;
}

void tty_write(const char *buf, int len)
{
    if (tty_tx)
        tty_tx(buf, len);
}

/* A host libc has no cheap stream that reaches com_putchar, so this formats
 * into a buffer and hands the result to the shared translation. */
int com_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return n;
    int w = (n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1;
    com_crlf_write(buf, w);
    return n;
}

/* The console's task on a machine whose console is the terminal the walk
 * already reaches: nothing, until a host puts a wire on it. Every pass, like
 * every other task, and gated on the one condition that means there is
 * nothing to do -- no room. A machine that is not reading fills the ring and
 * this stops asking; a machine that is reading gets the wire's full rate
 * rather than a ring a frame. */
void com_task(void)
{
    if (!tty_rx)
        return;
    size_t room = com_uart_free();
    if (!room)
        return;
    char buf[COM_RING_SIZE];
    if (room > sizeof buf)
        room = sizeof buf;
    size_t n = tty_rx(buf, room);
    if (n)
        com_uart_push(buf, n);
}
