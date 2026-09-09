/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host's stdio on this machine's console. What arrives is pushed at the
 * console's UART source, where a serial console's bytes arrive on real
 * hardware, so the registers, the line editor and a raw TTY: read all see it.
 * Nothing is translated on the way in; translating host text is the
 * clipboard's business.
 *
 * stdin and stdout are decided apart. A terminal on stdin is taken raw and
 * read as a console, where a 0x03 raises the RIA's SIGINT; a pipe or a file
 * is read as a stream, where 0x03 is an ordinary byte. Only a terminal that
 * is stdout's as well takes the machine's screen, because a terminal on stdin
 * with a file on stdout is a pipeline and the screen does not belong in the
 * file.
 */

#include "host/sokol/cli/console.h"
#include "host/sokol/cli/streams.h"
#include "core/api/std.h"
#include "core/com/com.h"
#include "core/sys/com_term.h"
#include "core/com/tty.h"
#include "core/vga/vga_emu.h"
#include "osal/console.h"
#include <stdbool.h>
#include <stdio.h>

static bool stdin_closed;
/* Written since the last flush. A program that draws with escapes and never
 * prints a newline would otherwise sit in the buffer unseen. */
static bool tx_pending;

static size_t stdin_rx(char *buf, size_t max)
{
    size_t n = os_console_read(buf, max);
    /* A read that finds nothing is the moment the machine has drawn all it
     * was going to, so a prompt with no newline after it reaches the screen
     * here. */
    if (!n && tx_pending)
    {
        tx_pending = false;
        fflush(stdout);
    }
    if (n || !os_console_ended())
        return n;
    /* End of file is reported only while a read is outstanding and the input
     * rings are empty, because std_stdin_eof gives that read up and bytes
     * still queued would never reach it. */
    if (!stdin_closed && std_stdin_waiting() && com_input_idle())
    {
        stdin_closed = true;
        std_stdin_eof();
    }
    return 0;
}

static void console_tx(const char *buf, int len)
{
    tx_pending = !streams_write(stdout, buf, len);
    if (ferror(stdout))
        os_console_break_ask(); /* the reader has gone */
}

bool console_open(void)
{
    bool typed = os_console_stdin_is_terminal();
    bool terminal = os_console_is_terminal();
    if (typed)
        os_console_raw(true);
    tty_set_wire(terminal ? console_tx : NULL, stdin_rx, !typed);
    if (!terminal)
        return false;
    /* Two terminals must not both answer a program's query. The host's is
     * the one the program can see, so the emulated one stops answering and
     * goes on drawing. */
    com_suppress_term_reply(true);
    /* The program's stderr already reaches this screen through console_tx,
     * so a second copy on the host's stderr would print everything twice. A
     * redirected stderr is another destination and keeps its copy. */
    if (os_console_stderr_is_terminal())
        com_set_stderr_sink(NULL);
    return true;
}

void console_idle(void)
{
    if (!std_stdin_waiting() || !com_input_idle())
        return;
    fflush(stdout);
    os_console_wait(VGA_FRAME_NS);
}

