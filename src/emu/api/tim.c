/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/host/host.h"
#include "ria/api/oem.h"
#include "ria/api/tim.h"

/* Wall-clock offset in seconds. Allows setting time without changing host clock. */
static int64_t tim_time_offset;

// Cold boot: adopt the host timezone/locale.
void tim_init(void)
{
    os_locale_reset();
    tzset(); /* populate tzname for strftime %Z from the host timezone */
}

bool tim_get_time(struct timespec *ts)
{
    ts->tv_sec = (time_t)((int64_t)time(NULL) + tim_time_offset);
    ts->tv_nsec = 0;
    return true;
}

bool tim_set_time(const struct timespec *ts)
{
    tim_time_offset = (int64_t)ts->tv_sec - (int64_t)time(NULL);
    return true;
}

void tim_get_time_res(struct timespec *ts)
{
    ts->tv_sec = 1;
    ts->tv_nsec = 0;
}

/* POSIX names the tzset() globals differently in every libc, so derive them:
 * probe both solstices, standard time is whichever sits further west. */
static void tim_tz_probe(long *std_west, int *daylight)
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

int tim_get_tz_daylight(void)
{
    long west;
    int daylight;
    tim_tz_probe(&west, &daylight);
    return daylight;
}

long tim_get_tz_offset(void)
{
    long west;
    int daylight;
    tim_tz_probe(&west, &daylight);
    return west;
}

/* strftime in the host locale, then UTF-8 -> OEM into dst (max bytes). */
size_t tim_strftime(char *dst, size_t max, const char *format,
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
