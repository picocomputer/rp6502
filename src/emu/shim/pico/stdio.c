/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pico_stdio layer, which on hardware is Pico SDK code: putchar/printf
 * land here (via the pico/stdlib.h shim), take the CRLF translation the SDK
 * performs above the firmware's com driver, and feed com's terminal wire.
 * The vendored terminal registers itself through stdio_set_driver_enabled
 * exactly as on hardware.
 */

#include "emu/sys/com.h"
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include <stdarg.h>
#include <stdio.h>

void stdio_set_driver_enabled(stdio_driver_t *driver, bool enabled)
{
    com_set_term_out(enabled && driver ? driver->out_chars : NULL);
}

/* Replicate pico stdio CRLF translation: a bare '\n' becomes "\r\n". Applied
 * unconditionally — the firmware analog flag (com_stdio_driver.crlf_enabled)
 * is constant true. */
static void stdio_out_chars_crlf(const char *buf, int len)
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

int stdio_putchar(int c)
{
    char ch = (char)c;
    stdio_out_chars_crlf(&ch, 1);
    return (int)(unsigned char)c;
}

int stdio_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return n;
    int w = (n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1;
    stdio_out_chars_crlf(buf, w);
    return n;
}
