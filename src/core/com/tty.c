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
#include "core/sys/com.h"
#include "core/sys/timer.h"
#include "machine.h"

#include <stdarg.h>
#include <stdio.h>

/* The host's end of the wire, when the host has one. */
static void (*tty_tx)(const char *buf, int len);
static size_t (*tty_rx)(char *buf, size_t max);
static bool tty_stream;
/* The hold on a console wire, from the pass that first found no room. */
static bool tty_held;
static timer_mach_t tty_hold;

void tty_set_wire(void (*tx)(const char *buf, int len),
                  size_t (*rx)(char *buf, size_t max), bool stream)
{
    tty_tx = tx;
    tty_rx = rx;
    tty_stream = stream;
    tty_held = false;
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
 * every other task. While the ring has room the wire is asked for what fits,
 * so a reader that keeps up loses nothing and gets the wire's full rate. A
 * full ring means nobody is reading: a stream is left where it is, which is
 * the backpressure that keeps a pipe whole, and a console is held for the
 * hold and then read to drop, because a Ctrl-C typed behind the type-ahead
 * has to be seen whether or not anyone will ever read the rest. */
void com_task(void)
{
    if (!tty_rx)
        return;
    char buf[COM_RING_SIZE];
    size_t room = com_uart_free();
    if (room)
    {
        tty_held = false;
        size_t n = tty_rx(buf, room < sizeof buf ? room : sizeof buf);
        if (!n)
            return;
        if (tty_stream)
            com_stream_push(buf, n);
        else
            com_uart_push(buf, n);
        return;
    }
    if (tty_stream)
        return;
    if (!tty_held)
    {
        tty_held = true;
        tty_hold = timer_mach_in_ms(COM_WIRE_HOLD_MS);
    }
    if (!timer_mach_passed(tty_hold))
        return;
    size_t n = tty_rx(buf, sizeof buf);
    if (n)
        com_uart_push(buf, n);
}
