/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host's stdio on this machine's console wire. What arrives is pushed at
 * the console's UART source, where a serial console's bytes arrive on real
 * hardware, so the registers, the line editor and a raw TTY: read all reach
 * it. Nothing is translated on the way in: a wire carries what the far end
 * sent. Translating is the clipboard's business, which is host text.
 */

#include "host/sokol/cli/console.h"
#include "host/sokol/cli/streams.h"
#include "core/api/std.h"
#include "core/com/com.h"
#include "core/sys/com_term.h"
#include "core/com/tty.h"
#include "core/str/oem.h"
#include "core/vga/vga_emu.h"
#include "osal/console.h"
#include <stdbool.h>
#include <stdio.h>

static bool stdin_closed;
/* Written since the last flush. A program that draws with escapes and never
 * prints a newline would otherwise sit in the buffer unseen. */
static bool tx_pending;

/* Whatever the far end sent, byte for byte. A wire carries no encoding and
 * spells no line ends: what a terminal or a pipe put on it is what a UART
 * would have delivered, and the machine reads it the way it reads one.
 * Translating belongs to the clipboard, which is host text. */
static size_t stdin_rx(char *buf, size_t max)
{
    size_t n = os_console_read(buf, max);
    /* Nothing arriving is the moment the machine has drawn whatever it was
     * going to. This is the only flush a windowed run ever gets, and it is
     * where a prompt with no newline after it reaches the screen. */
    if (!n && tx_pending)
    {
        tx_pending = false;
        fflush(stdout);
    }
    if (n || !os_console_ended())
        return n;
    /* Only once the wire has drained and a cooked read is genuinely starved:
     * an end of file found here can then cancel nothing that was coming. */
    if (!stdin_closed && std_stdin_waiting() && com_input_idle())
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
    tx_pending = true;
    if (line)
    {
        fflush(stdout);
        tx_pending = false;
    }
    if (ferror(stdout))
        os_console_break_ask(); /* the reader went away */
}

bool console_open(void)
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

void console_idle(void)
{
    if (!std_stdin_waiting() || !com_input_idle())
        return;
    fflush(stdout);
    os_console_wait(VGA_FRAME_NS);
}

