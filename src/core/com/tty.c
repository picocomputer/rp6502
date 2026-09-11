/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/tty.h"
#include "core/com/com.h"
#include "core/sys/com.h"
#include "core/sys/timer.h"
#include "machine.h"

#include <stdarg.h>
#include <stdio.h>

static void (*tty_tx)(const char *buf, int len);
static size_t (*tty_rx)(char *buf, size_t max);
static bool tty_stream;
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

/* A stream wire is not read while the ring is full. A console wire is left for
 * COM_WIRE_HOLD_MS and then read into the full ring, where the bytes are
 * dropped, because a Ctrl-C typed behind the type-ahead has to be seen even
 * though the rest of what was typed is lost. */
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
