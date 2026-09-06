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
