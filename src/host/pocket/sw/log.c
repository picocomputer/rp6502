/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "log.h"
#include "mmio.h"
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

/* Nothing a program prints is written to the debug port. */
void log_init(void)
{
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
