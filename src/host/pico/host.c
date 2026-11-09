/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/host.h"
#include "osal/os.h"
#include "core/sys/debug_log.h"
#include "machine.h"

#include <littlefs/lfs_util.h>
#include <pico/time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void HOST_IN_RAM_FUNC host_log(int level, const char *category, const char *fmt, ...)
{
    static const char *const names[] = RP6502_LOG_LEVEL_NAMES;
    printf("%s %s: ", names[level], category);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    size_t n = strlen(fmt);
    if (!n || fmt[n - 1] != '\n')
        putchar('\n');
}

#if CFG_TUSB_DEBUG
static void HOST_IN_RAM_FUNC rp6502_log_tusb_line(const char *line)
{
#if CFG_TUSB_DEBUG == 1
    RP6502_LOG(tinyusb, ERROR, "%s", line);
#else
    RP6502_LOG(tinyusb, DEBUG, "%s", line);
#endif
}

/* TinyUSB prints a line in pieces, which are gathered here and logged a line
 * at a time. A line that fills the buffer is logged as it stands. */
int HOST_IN_RAM_FUNC rp6502_log_tusb(const char *fmt, ...)
{
    static char line[128];
    static size_t len;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line + len, sizeof(line) - len, fmt, ap);
    va_end(ap);
    if (n < 0)
        return n;
    size_t room = sizeof(line) - 1 - len;
    len += (size_t)n < room ? (size_t)n : room;
    char *start = line;
    char *end;
    while ((end = memchr(start, '\n', len - (size_t)(start - line))))
    {
        if (end > start && end[-1] == '\r')
            end[-1] = 0;
        *end = 0;
        rp6502_log_tusb_line(start);
        start = end + 1;
    }
    len -= (size_t)(start - line);
    memmove(line, start, len);
    line[len] = 0;
    if (len == sizeof(line) - 1)
    {
        rp6502_log_tusb_line(line);
        len = 0;
    }
    return n;
}
#endif

uint64_t HOST_IN_RAM_FUNC host_clock_us(void)
{
    return time_us_64();
}

static uint32_t seed;
static bool seed_taken;

uint32_t host_seed(void)
{
    if (!seed_taken)
    {
        seed = os_random();
        seed_taken = true;
    }
    return seed;
}

uint32_t host_crc32(uint32_t crc, const void *buf, size_t len)
{
    return ~lfs_crc(~crc, buf, len);
}
