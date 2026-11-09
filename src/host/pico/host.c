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
