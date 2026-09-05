/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "host/sokol/cli/streams.h"
#include "core/api/std.h"
#include "core/com/com.h"
#include "core/hid/vtkeys.h"
#include "core/str/oem.h"
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

/* Only when the read is genuinely starved: the last paste has dripped and the
 * line editor has taken every byte of it, so an end of file found here can
 * only cancel a read nothing is on its way to. */
void streams_feed_stdin(void)
{
    if (!std_stdin_waiting() || vtkeys_paste_busy() ||
        com_keyboard_free() != COM_RING_SIZE - 1)
        return;
    fflush(stdout);
    char line[4096];
    if (!fgets(line, sizeof line - 1, stdin))
    {
        std_stdin_eof();
        return;
    }
    /* A last line without a newline is still a line. */
    size_t n = strlen(line);
    if ((!n || line[n - 1] != '\n') && feof(stdin))
    {
        line[n++] = '\n';
        line[n] = 0;
    }
    vtkeys_paste(line);
}
