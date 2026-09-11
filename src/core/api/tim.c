/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/os.h"
#include "core/str/oem.h"
#include "core/api/tim.h"

void tim_init(void)
{
    os_locale_reset();
    tzset(); /* populate tzname for strftime %Z from the host timezone */
}

void tim_stop(void)
{
    os_locale_free();
}

bool tim_get_time(struct timespec *ts)
{
    ts->tv_sec = time(NULL);
    ts->tv_nsec = 0;
    return true;
}

/* The host owns its clock, so a program that asks to move it is refused
 * rather than given an offset only this machine would carry. */
bool tim_set_time(const struct timespec *ts)
{
    (void)ts;
    return false;
}

void tim_get_time_res(struct timespec *ts)
{
    ts->tv_sec = 1;
    ts->tv_nsec = 0;
}

bool tim_localtime(time_t t, struct tm *out)
{
    return os_localtime(t, out);
}

bool tim_gmtime(time_t t, struct tm *out)
{
    return os_gmtime(t, out);
}

size_t tim_strftime(char *dst, size_t max, const char *format,
                    const struct tm *tm)
{
    /* A throwaway mktime fills tm_gmtoff and tm_zone from the host timezone,
     * where the host's struct tm has them, so %z and %Z match the firmware's
     * newlib, which derives them from the timezone plus tm_isdst. It runs on
     * its own copy because mktime also recomputes tm_wday and tm_yday, and %a
     * and %A must come from the values the 6502 pushed. */
    struct tm zoned = *tm, probe = *tm;
    if (mktime(&probe) != (time_t)-1)
        os_tm_apply_zone(&zoned, &probe);
    char utf8[512];
    size_t un = os_strftime_local(utf8, sizeof utf8, format, &zoned);
    /* strftime returns 0 on overflow and leaves the buffer unspecified, so a
     * terminator is forced before the UTF-8 walk below reads it. */
    utf8[un < sizeof utf8 ? un : sizeof utf8 - 1] = 0;
    size_t pos = 0;
    const char *p = utf8;
    unsigned char ch;
    while ((ch = oem_from_utf8_next(&p)))
    {
        /* The render stops one byte short of max, and an overflow discards
         * the whole render, both to match host/pico/ria/api/tim.c; the byte it
         * spends on a terminator is left unwritten here. */
        if (pos + 1 >= max)
            return 0;
        dst[pos++] = ch;
    }
    return pos;
}
