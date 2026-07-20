/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/host/host.h"
#include "ria/api/oem.h"
#include "ria/sys/sys.h"
#include "pico/time.h"
#include <string.h>

/* Wall-clock offset in seconds. Allows setting time without changing host clock. */
static int64_t sys_time_offset;

/* Master-clock time (us) at the current program's start. */
static uint64_t sys_start_us;

void sys_init(void)
{
    os_locale_reset();
    tzset(); /* populate tzname for strftime %Z from the host timezone */
}

void sys_run(void)
{
    sys_start_us = time_us_64();
}

/* 6502 run time from the one master clock, so it is a reproducible function of
 * the frame count (the vendored atr.c reads it for the s/ds/cs/ms attributes). */
uint32_t sys_get_run(uint32_t us_per_tick)
{
    return (uint32_t)((time_us_64() - sys_start_us) / us_per_tick);
}

bool sys_get_time(struct timespec *ts)
{
    ts->tv_sec = (time_t)((int64_t)time(NULL) + sys_time_offset);
    ts->tv_nsec = 0;
    return true;
}

bool sys_set_time(const struct timespec *ts)
{
    sys_time_offset = (int64_t)ts->tv_sec - (int64_t)time(NULL);
    return true;
}

void sys_get_time_res(struct timespec *ts)
{
    ts->tv_sec = 1;
    ts->tv_nsec = 0;
}

/* POSIX names the tzset() globals differently in every libc, so derive them:
 * probe both solstices, standard time is whichever sits further west. */
static void sys_tz_probe(long *std_west, int *daylight)
{
    static const time_t probe[2] = {1735732800, 1751371200}; /* 2025 Jan/Jul 1, noon UTC */
    long west[2];
    int isdst[2];
    for (int i = 0; i < 2; i++)
    {
        struct tm local, gm;
        os_localtime(probe[i], &local);
        os_gmtime(probe[i], &gm);
        gm.tm_isdst = local.tm_isdst;
        west[i] = (long)(mktime(&gm) - mktime(&local));
        isdst[i] = local.tm_isdst > 0;
    }
    *std_west = west[0] > west[1] ? west[0] : west[1];
    *daylight = isdst[0] || isdst[1];
}

int sys_get_tz_daylight(void)
{
    long west;
    int daylight;
    sys_tz_probe(&west, &daylight);
    return daylight;
}

long sys_get_tz_offset(void)
{
    long west;
    int daylight;
    sys_tz_probe(&west, &daylight);
    return west;
}

/* strftime in the host locale, then UTF-8 -> OEM into dst (max bytes). */
size_t sys_strftime(char *dst, size_t max, const char *format,
                    const struct tm *tm)
{
    /* Populate tm_gmtoff/tm_zone from the host timezone (a probe mktime) so %z and
     * %Z match the firmware's newlib, which derives them from the timezone plus
     * tm_isdst. Done on a copy so the wire's own tm_wday/tm_yday still drive
     * %a/%A rather than being recomputed. */
    struct tm zoned = *tm, probe = *tm;
    if (mktime(&probe) != (time_t)-1)
        os_tm_apply_zone(&zoned, &probe);
    char utf8[512];
    size_t un = os_strftime_local(utf8, sizeof utf8, format, &zoned);
    /* On overflow strftime returns 0 and leaves the buffer unspecified; force a
     * terminator so the UTF-8 walk below can't run off the end. */
    utf8[un < sizeof utf8 ? un : sizeof utf8 - 1] = 0;
    size_t pos = 0;
    const char *p = utf8;
    unsigned char ch;
    while ((ch = oem_from_utf8_next(&p)))
    {
        if (pos + 1 >= max) /* reserve a terminator; overflow discards the whole render (firmware parity) */
            return 0;
        dst[pos++] = ch;
    }
    return pos;
}
