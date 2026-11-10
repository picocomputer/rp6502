/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/pocket/sw/log.h"
#include "host/pocket/sw/mmio.h"
#include "core/com/manifold.h"
#include "core/sys/debug_log.h"

#include <stdarg.h>
#include <stdio.h>

static int log_putc(char c, FILE *f)
{
    (void)f;
    MMIO_CONSOLE = (uint8_t)c;
    return c;
}

static FILE log_port = FDEV_SETUP_STREAM(log_putc, NULL, NULL, _FDEV_SETUP_WRITE);

static void log_console(const char *buf, int len)
{
    for (int i = 0; i < len; i++)
        MMIO_CONSOLE = (uint8_t)buf[i];
}

/* Every byte the console sends goes to the debug port too, where the test
 * bench reads it. */
void log_init(void)
{
    manifold_set_tx_tap(log_console);
}

void host_log(int level, const char *category, const char *fmt, ...)
{
    static const char *const names[] = RP6502_LOG_LEVEL_NAMES;
    fprintf(&log_port, "%s %s: ", names[level], category);
    va_list va;
    va_start(va, fmt);
    vfprintf(&log_port, fmt, va);
    va_end(va);
    fputc('\n', &log_port);
}
