/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/sys/log.h"
#include <stdarg.h>
#include <stdio.h>

/* Long enough for a path and a sentence about it; a diagnostic that would
 * not fit is worth reading truncated. */
#define LOG_MAX 512

static log_sink_t log_sink;

static void log_stderr(const char *msg)
{
    fprintf(stderr, "rp6502-emu: %s\n", msg);
}

void log_set_sink(log_sink_t sink)
{
    log_sink = sink;
}

void log_error(const char *fmt, ...)
{
    char msg[LOG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (log_sink)
        log_sink(msg);
    else
        log_stderr(msg);
}
