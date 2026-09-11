/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The bench's answer to core/sys/debug_log.h, beside its answer to
 * host/host.h: a test's machine says its lines on stderr.
 */

#include "core/sys/debug_log.h"
#include <stdarg.h>
#include <stdio.h>

void host_log(int level, const char *category, const char *fmt, ...)
{
    static const char *const names[] = RP6502_LOG_LEVEL_NAMES;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s %s: ", names[level], category);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
