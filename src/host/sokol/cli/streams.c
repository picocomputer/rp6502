/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "host/sokol/cli/streams.h"
#include "core/com/com.h"
#include "core/str/oem.h"
#include "osal/console.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Host streams carry host encoding, so OEM bytes expand to UTF-8 -- in
 * chunks, because a write per byte is a syscall per byte. */
bool streams_write(FILE *f, const char *buf, int len)
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
            fwrite(out, 1, (size_t)n, f);
            n = 0;
        }
    }
    if (n)
        fwrite(out, 1, (size_t)n, f);
    if (line)
        fflush(f);
    return line;
}

/* Only the stream the machine's output is on may end the run by going away.
 * The diagnostic copies -- the program's stderr, EMU_ECHO's console -- write
 * to a stream nobody may be reading, and a failure there is not a reason to
 * stop a machine that is working. */
static void streams_stdout_tap(int fd, const char *buf, int len)
{
    if (fd != 1)
        return;
    streams_write(stdout, buf, len);
    if (ferror(stdout))
        os_console_break_ask(); /* the reader went away */
}

void streams_mirror_stdout(void)
{
    com_set_std_tap(streams_stdout_tap);
}

void streams_stderr(const char *buf, int len)
{
    streams_write(stderr, buf, len);
}

void streams_mirror_stderr(void)
{
    com_set_stderr_sink(streams_stderr);
}
