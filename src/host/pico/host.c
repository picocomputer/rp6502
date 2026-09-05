/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What a Pico firmware says about itself. Both compile this; the VGA takes
 * only the clock, and the seed and the CRC go unreferenced there, so the
 * linker drops them along with the littlefs they would have reached.
 */

#include "host/host.h"
#include "osal/os.h"
#include "core/sys/debug_log.h"

#include <littlefs/lfs_util.h>
#include <pico/time.h> /* time_us_64 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The console is the only place a Pico has. This is the one sink that sees a
 * vendor's own formats, and cyw43's end in a newline of their own. */
void host_log(int level, const char *category, const char *fmt, ...)
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
/* TinyUSB prints a line in pieces and never says its level, so the pieces
 * are gathered until the newline. At its level 1 only failures print, which
 * is ERROR; above that it is describing the traffic. */
int rp6502_log_tusb(const char *fmt, ...)
{
    static char *line;
    static size_t cap, len;
    va_list ap;
    va_start(ap, fmt);
    va_list sizing;
    va_copy(sizing, ap);
    int n = vsnprintf(NULL, 0, fmt, sizing);
    va_end(sizing);
    if (n < 0)
    {
        va_end(ap);
        return n;
    }
    if (len + (size_t)n + 1 > cap)
    {
        cap = len + (size_t)n + 1;
        line = realloc(line, cap);
    }
    vsnprintf(line + len, cap - len, fmt, ap);
    va_end(ap);
    len += (size_t)n;
    char *start = line;
    char *end;
    while ((end = memchr(start, '\n', len - (size_t)(start - line))))
    {
        if (end > start && end[-1] == '\r')
            end[-1] = 0;
        *end = 0;
#if CFG_TUSB_DEBUG == 1
        RP6502_LOG(tinyusb, ERROR, "%s", start);
#else
        RP6502_LOG(tinyusb, DEBUG, "%s", start);
#endif
        start = end + 1;
    }
    len -= (size_t)(start - line);
    memmove(line, start, len);
    return n;
}
#endif

/* TIMER0's raw 64-bit microsecond counter, free-running since reset. */
uint64_t host_clock_us(void)
{
    return time_us_64();
}

/* Nothing overrides a board -- no command line, no fixture -- so the seed is
 * the RNG's, drawn once and held. */
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
    // littlefs's CRC table is already linked; reuse it, don't add a second.
    return ~lfs_crc(~crc, buf, len);
}
