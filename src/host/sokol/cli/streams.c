/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "host/sokol/cli/streams.h"
#include "core/api/std.h"
#include "core/com/com.h"
#include "core/com/tty.h"
#include "core/str/oem.h"
#include "core/vga/vga_emu.h"
#include "osal/os.h"
#include "core/sys/debug_log.h"
#ifdef EMU_WITH_DEBUGGER
#include "core/dap/dap.h"
#endif
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The machine's own lines: host stderr, or the debugger's stderr channel
 * when a client has the console. */
void host_log(int level, const char *category, const char *fmt, ...)
{
    static const char *const names[] = RP6502_LOG_LEVEL_NAMES;
    va_list ap;
    va_start(ap, fmt);
#ifdef EMU_WITH_DEBUGGER
    if (dap_is_active())
    {
        dap_log(level, category, fmt, ap);
        va_end(ap);
        return;
    }
#endif
    fprintf(stderr, "%s %s: ", names[level], category);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* Host streams carry host encoding, so OEM bytes expand to UTF-8. A line is
 * flushed here because Windows has no line buffering. */
static void streams_stdout_tap(int fd, const char *buf, int len)
{
    if (fd != 1)
        return;
    char out[3 * 128];
    int n = 0;
    bool line = false;
    for (int i = 0; i < len; i++)
    {
        line |= buf[i] == '\n';
        n += oem_to_utf8_char((unsigned char)buf[i], out + n);
        if (n > (int)sizeof(out) - 3)
        {
            fwrite(out, 1, (size_t)n, stdout);
            n = 0;
        }
    }
    if (n)
        fwrite(out, 1, (size_t)n, stdout);
    if (line)
        fflush(stdout);
}

void streams_mirror_stdout(void)
{
    com_set_std_tap(streams_stdout_tap);
}

/* ---- the host's stdio as this machine's console wire ---- */

/* What the far end sent and this end could not use yet: a UTF-8 sequence or
 * a CR whose LF may be in the next read. Never more than one sequence. */
static char stdin_carry[8];
static size_t stdin_carry_len;
/* Whether the wire has delivered anything since the last line end, so an
 * input that stops without one still finishes its line. */
static bool stdin_mid_line;
/* A pipe or a file carries host text, whose line endings are the host's. A
 * terminal is the wire itself and already sends what a line editor reads, so
 * every byte it sends is passed on as it was struck. */
static oem_run_t stdin_run;
static bool stdin_closed;

static size_t stdin_rx(char *buf, size_t max)
{
    /* No more than the ring can take: a sequence never grows, so max bytes
     * in convert to at most max out, and what is left over is only ever the
     * tail of a sequence the read cut in half. */
    char raw[256];
    size_t held = stdin_carry_len;
    size_t want = max < sizeof raw - held ? max : sizeof raw - held;
    memcpy(raw, stdin_carry, held);
    size_t got = want ? os_stdin_read(raw + held, want) : 0;
    size_t have = held + got;
    bool end = os_stdin_ended();

    size_t taken = 0;
    size_t n = have ? oem_from_utf8_run(&stdin_run, raw, have, end,
                                        buf, max, &taken)
                    : 0;
    stdin_carry_len = have - taken;
    if (stdin_carry_len > sizeof stdin_carry)
        stdin_carry_len = 0; /* nothing that long is a sequence; drop it */
    memcpy(stdin_carry, raw + taken, stdin_carry_len);
    for (size_t i = 0; i < n; i++)
        stdin_mid_line = buf[i] != '\r';

    if (!end || stdin_carry_len || n)
        return n;
    /* The far end is gone and everything it sent has been converted. A last
     * line it never ended is still a line, so it gets its return before the
     * read that was waiting on it is told there is nothing more. */
    if (stdin_mid_line && max)
    {
        stdin_mid_line = false;
        buf[0] = '\r';
        return 1;
    }
    /* Only once the wire has drained and a cooked read is genuinely starved:
     * an end of file found here can then cancel nothing that was coming. */
    if (!stdin_closed && std_stdin_waiting() && com_uart_free() == COM_RING_SIZE - 1)
    {
        stdin_closed = true;
        std_stdin_eof();
    }
    return 0;
}

/* The machine's terminal stream, out on the host's, where a real terminal is
 * reading it. Already CRLF-translated; the encoding is all that changes. */
static void console_tx(const char *buf, int len)
{
    char out[3 * 128];
    int n = 0;
    bool line = false;
    for (int i = 0; i < len; i++)
    {
        line |= buf[i] == '\n';
        n += oem_to_utf8_char((unsigned char)buf[i], out + n);
        if (n > (int)sizeof(out) - 3)
        {
            fwrite(out, 1, (size_t)n, stdout);
            n = 0;
        }
    }
    if (n)
        fwrite(out, 1, (size_t)n, stdout);
    if (line)
        fflush(stdout);
}

bool streams_console_open(void)
{
    bool terminal = os_stdin_is_terminal();
    stdin_run.newlines = !terminal;
    if (!terminal)
    {
        tty_set_wire(NULL, stdin_rx);
        return false;
    }
    os_stdin_raw(true);
    tty_set_wire(console_tx, stdin_rx);
    /* Two terminals must not both answer a program's query. The one at the
     * far end is the one the program can see, so the emulated one stops
     * answering and goes on drawing. */
    com_suppress_term_reply(true);
    /* Its stderr reaches the same screen through the stream above; a second
     * copy on the host's would print everything twice. A redirected stderr
     * is another destination and still gets its own. */
    if (os_stderr_is_terminal())
        tty_set_stderr_host(false);
    return true;
}

void streams_stdin_idle(void)
{
    if (!std_stdin_waiting() || com_uart_free() != COM_RING_SIZE - 1)
        return;
    fflush(stdout);
    os_stdin_wait(VGA_FRAME_NS);
}

