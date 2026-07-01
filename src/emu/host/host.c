/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host glue for the vendored terminal: captures term.c's stdio driver so
 * the RIA stdout syscall (and the reused firmware putchar/printf) can feed
 * bytes straight into it, applying the firmware's CRLF translation.
 */

#include "emu/host/host.h"
#include "emu/sys/com.h"
#include "emu/sys/sys.h"
#include "aud/bel.h"
#include "pico/stdio/driver.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* The virtual clock (emu_now_us) lives in sys.c now — it reads the one master
 * clock that the 6502 ticks advance, so it stays in lockstep with PHI2/VGA. */

/* ------------------------------------------------------------------ */
/* stdio driver capture                                                */
/* ------------------------------------------------------------------ */

static stdio_driver_t *g_stdio;

void stdio_set_driver_enabled(stdio_driver_t *driver, bool enabled)
{
    g_stdio = enabled ? driver : NULL;
}

/* Optional tap on the raw terminal stream (set by tests to capture output). */
static void (*g_stdout_tap)(const char *buf, int len);

void emu_set_stdout_tap(void (*tap)(const char *buf, int len))
{
    g_stdout_tap = tap;
}

/* Echo/tap/bell processing common to the translated and raw sinks. The bell
 * rings on any BEL in the terminal stream, like the firmware's TX-drain scan
 * (ria/sys/com.c). */
static void stdout_taps(const char *buf, int len)
{
    /* Bring-up aid: EMU_ECHO mirrors the terminal stream to the host's stderr
     * so the program's text output is visible without rendering the frame. */
    static int echo = -1;
    if (echo < 0)
        echo = getenv("EMU_ECHO") ? 1 : 0;
    if (echo)
        fwrite(buf, 1, (size_t)len, stderr);
    if (g_stdout_tap)
        g_stdout_tap(buf, len);
    if (com_get_bel())
        for (int i = 0; i < len; i++)
            if (buf[i] == '\a')
                bel_add(&bel_teletype);
}

void emu_stdout_write(const char *buf, int len)
{
    static char last;
    stdout_taps(buf, len);
    if (!g_stdio || !g_stdio->out_chars)
        return;
    /* Replicate pico stdio CRLF translation: a bare '\n' becomes "\r\n". */
    if (!g_stdio->crlf_enabled)
    {
        g_stdio->out_chars(buf, len);
        return;
    }
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
            g_stdio->out_chars(out, n);
            n = 0;
        }
    }
    if (n)
        g_stdio->out_chars(out, n);
}

void emu_stdout_write_raw(const char *buf, int len)
{
    stdout_taps(buf, len);
    if (g_stdio && g_stdio->out_chars)
        g_stdio->out_chars(buf, len);
}

/* ------------------------------------------------------------------ */
/* Firmware stdout routing (putchar/printf -> terminal)                */
/* ------------------------------------------------------------------ */

/* The reused firmware sources (rln.c) echo input and emit ANSI handshakes via
 * putchar/printf, redirected here by the pico/stdlib.h shim so they reach the
 * same terminal sink as the 6502's stdout syscall. */

int emu_term_putchar(int c)
{
    char ch = (char)c;
    emu_stdout_write(&ch, 1);
    return (int)(unsigned char)c;
}

int emu_term_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return n;
    int w = (n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1;
    emu_stdout_write(buf, w);
    return n;
}
