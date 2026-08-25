/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's end of the console wire. There is no wire: the terminal is
 * rendered in the same process, so the only thing here is the bring-up mirror
 * and the register-window byte the RIA model stages.
 */

#include "core/com/tty.h"
#include "core/api/oem.h"
#include "core/ria/ria.h"

#include "core/com/com.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void tty_write(const char *buf, int len)
{
    /* EMU_ECHO mirrors the terminal stream to the host's stderr, so a
     * program's output is visible without rendering a frame. Host streams
     * carry host encoding, so OEM bytes expand to UTF-8. */
    static int echo = -1;
    if (echo < 0)
        echo = getenv("EMU_ECHO") ? 1 : 0;
    if (!echo)
        return;
    for (int i = 0; i < len; i++)
    {
        char enc[3];
        fwrite(enc, 1, (size_t)oem_to_utf8_char((unsigned char)buf[i], enc), stderr);
    }
}

/* A read of $FFE0 pulls a byte into the $FFE2 latch to answer the ready bit;
 * this is where it comes back. */
bool tty_reg_reclaim(char *out)
{
    return ria_reg_rx_reclaim(out);
}

/* A host libc has no cheap stream that reaches com_putchar, so this formats
 * into a buffer and hands the result to the shared translation. */
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
