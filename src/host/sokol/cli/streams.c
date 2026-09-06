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
#include "osal/console.h"
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
    if (ferror(stdout))
        os_console_break_ask(); /* the reader went away */
}

void streams_mirror_stdout(void)
{
    com_set_std_tap(streams_stdout_tap);
}

/* ---- the host's stdio as this machine's console wire ---- */

static bool stdin_closed;

/* Whatever the far end sent, byte for byte. A wire carries no encoding and
 * spells no line ends: what a terminal or a pipe put on it is what a UART
 * would have delivered, and the machine reads it the way it reads one.
 * Translating belongs to the clipboard, which is host text. */
static size_t stdin_rx(char *buf, size_t max)
{
    size_t n = os_console_read(buf, max);
    if (n || !os_console_ended())
        return n;
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
    if (ferror(stdout))
        os_console_break_ask(); /* the reader went away */
}

bool streams_console_open(void)
{
    bool terminal = os_console_is_terminal();
    if (!terminal)
    {
        tty_set_wire(NULL, stdin_rx);
        return false;
    }
    os_console_raw(true);
    tty_set_wire(console_tx, stdin_rx);
    /* Two terminals must not both answer a program's query. The one at the
     * far end is the one the program can see, so the emulated one stops
     * answering and goes on drawing. */
    com_suppress_term_reply(true);
    /* Its stderr reaches the same screen through the stream above; a second
     * copy on the host's would print everything twice. A redirected stderr
     * is another destination and still gets its own. */
    if (os_console_stderr_is_terminal())
        tty_set_stderr_host(false);
    return true;
}

void streams_stdin_idle(void)
{
    if (!std_stdin_waiting() || com_uart_free() != COM_RING_SIZE - 1)
        return;
    fflush(stdout);
    os_console_wait(VGA_FRAME_NS);
}

