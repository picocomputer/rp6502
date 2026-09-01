/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/os.h"
#include "core/str/oem.h"
#include "core/api/tim.h"

// Cold boot: adopt the host timezone/locale.
void tim_init(void)
{
    os_locale_reset();
    tzset(); /* populate tzname for strftime %Z from the host timezone */
}

bool tim_get_time(struct timespec *ts)
{
    ts->tv_sec = time(NULL);
    ts->tv_nsec = 0;
    return true;
}

/* The host's clock is the host's. A program that asks to move it is refused
 * rather than handed an offset this machine would carry and the wall would
 * not -- two clocks disagreeing is worse than one that will not move. */
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
