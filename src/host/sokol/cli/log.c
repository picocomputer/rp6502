/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's own log lines, which core/sys/debug_log.h asks every machine
 * to answer for. These are not a program's output, which goes wherever the
 * run points it.
 */

#include "core/sys/debug_log.h"
#include <stdbool.h>
#ifdef EMU_WITH_DEBUGGER
#include "core/dap/dap.h"
#endif
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

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
