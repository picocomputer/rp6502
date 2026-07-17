/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/api/clk.h"
#include "emu/compiler.h"
#include "emu/plat.h"
#include "emu/sys/mem.h"
#include "pico/time.h"
#include "api/api.h"
#include "api/clk.h"
#include "api/oem.h"
#include <string.h>
#include <time.h>

/* Wall-clock offset in seconds. Allows setting time without changing host clock. */
static int64_t g_time_offset;

/* Master-clock time (us) at the current program's start. clk_get_run counts from
 * here; clk_run re-anchors it on every program start (firmware clk_run parity). */
static uint64_t g_run_start_us;

void clk_reset(void)
{
    g_time_offset = 0;
    os_locale_reset();
    tzset(); /* populate tzname for strftime %Z from the host timezone */
}

// Re-anchor the 6502 run clock to now.
void clk_run(void)
{
    g_run_start_us = time_us_64();
}

/* 6502 run time since the current program started, in ticks of us_per_tick
 * microseconds, from the one master clock (the vendored atr.c reads it for the
 * s/ds/cs/ms attributes). */
uint32_t clk_get_run(uint32_t us_per_tick)
{
    return (uint32_t)((time_us_64() - g_run_start_us) / us_per_tick);
}

/* strftime in the host locale, then UTF-8 -> OEM into dst (max bytes). */
static size_t clk_strftime(char *dst, size_t max, const char *format,
                           const struct tm *tm)
{
    char utf8[512];
    size_t un = os_strftime_local(utf8, sizeof utf8, format, tm);
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

/* The 18-byte wire struct tm the 6502 libc pushes (9 int16, struct-tm order). */
EMU_PACK_BEGIN
struct EMU_PACKED clk_wire_tm
{
    int16_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon;
    int16_t tm_year, tm_wday, tm_yday, tm_isdst;
};
EMU_PACK_END
_Static_assert(18 == sizeof(struct clk_wire_tm), "wire struct tm");

static void clk_tm_to_wire(const struct tm *tm, struct clk_wire_tm *w)
{
    w->tm_sec = tm->tm_sec, w->tm_min = tm->tm_min, w->tm_hour = tm->tm_hour;
    w->tm_mday = tm->tm_mday, w->tm_mon = tm->tm_mon, w->tm_year = tm->tm_year;
    w->tm_wday = tm->tm_wday, w->tm_yday = tm->tm_yday, w->tm_isdst = tm->tm_isdst;
}

static void clk_wire_to_tm(const struct clk_wire_tm *w, struct tm *tm)
{
    memset(tm, 0, sizeof(*tm));
    tm->tm_sec = w->tm_sec, tm->tm_min = w->tm_min, tm->tm_hour = w->tm_hour;
    tm->tm_mday = w->tm_mday, tm->tm_mon = w->tm_mon, tm->tm_year = w->tm_year;
    tm->tm_wday = w->tm_wday, tm->tm_yday = w->tm_yday, tm->tm_isdst = w->tm_isdst;
}

/* op 0x3F: time_t time(void) — host wall clock plus any time_set offset. */
bool clk_api_time_get(void)
{
    int64_t sec = (int64_t)time(NULL) + g_time_offset;
    if (!api_push_n(&sec, sizeof(sec)))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

/* op 0x3E: int clock_settime — record the offset from the host clock. */
bool clk_api_time_set(void)
{
    uint64_t u;
    if (!api_pop_uint64_end(&u))
        return api_return_errno(API_EINVAL);
    g_time_offset = (int64_t)u - (int64_t)time(NULL);
    return api_return_ax(0);
}

static bool clk_api_to_tm(bool local)
{
    uint64_t u;
    if (!api_pop_uint64_end(&u))
        return api_return_errno(API_EINVAL);
    time_t t = (int64_t)u; /* 8-byte pushes carry the sign */
    struct tm tm;
    if (local)
        os_localtime(t, &tm);
    else
        os_gmtime(t, &tm);
    if (tm.tm_year < INT16_MIN || tm.tm_year > INT16_MAX)
        return api_return_errno(API_ERANGE);
    struct clk_wire_tm w;
    clk_tm_to_wire(&tm, &w);
    if (!api_push_n(&w, sizeof(w)))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

/* op 0x3A / 0x3B: struct tm *gmtime/localtime(const time_t *). */
bool clk_api_gmtime(void) { return clk_api_to_tm(false); }
bool clk_api_localtime(void) { return clk_api_to_tm(true); }

/* op 0x3C: time_t mktime(struct tm *). */
bool clk_api_mktime(void)
{
    struct clk_wire_tm w;
    if (!api_pop_n(&w, sizeof(w)) || xstack_ptr != XSTACK_SIZE)
        return api_return_errno(API_EINVAL);
    struct tm tm;
    clk_wire_to_tm(&w, &tm);
    time_t t = mktime(&tm);
    if (t == (time_t)-1)
        return api_return_errno(API_ERANGE);
    int64_t sec = t;
    if (!api_push_n(&sec, sizeof(sec)))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

/* op 0x3D: size_t strftime(char *, size_t, const char *fmt, struct tm *).
 * The xstack holds, from the top: the format string (+NUL), then the 18-byte
 * wire tm. Output composes below the format, then relocates to the top. */
bool clk_api_strftime(void)
{
    const char *format = (char *)&xstack[xstack_ptr];
    size_t max = xstack_ptr; /* output composes into xstack[0..max) */
    size_t format_size = strlen(format) + 1;
    if (format_size > XSTACK_SIZE - max)
        return api_return_errno(API_EINVAL);
    xstack_ptr = max + format_size;
    struct clk_wire_tm w;
    if (!api_pop_n(&w, sizeof(w)) || xstack_ptr != XSTACK_SIZE)
        return api_return_errno(API_EINVAL);
    if (w.tm_sec < 0 || w.tm_sec > 61 || w.tm_min < 0 || w.tm_min > 59 ||
        w.tm_hour < 0 || w.tm_hour > 23 || w.tm_mday < 1 || w.tm_mday > 31 ||
        w.tm_mon < 0 || w.tm_mon > 11 || w.tm_wday < 0 || w.tm_wday > 6 ||
        w.tm_yday < 0 || w.tm_yday > 365)
        return api_return_errno(API_EINVAL);
    struct tm tm;
    clk_wire_to_tm(&w, &tm);
    /* Populate tm_gmtoff/tm_zone from the host timezone (a probe mktime) so %z and
     * %Z match the firmware's newlib, which derives them from the timezone plus
     * tm_isdst. Done on a copy so the wire's own tm_wday/tm_yday still drive
     * %a/%A rather than being recomputed. */
    struct tm probe = tm;
    if (mktime(&probe) != (time_t)-1)
        os_tm_apply_zone(&tm, &probe);
    size_t n = clk_strftime((char *)xstack, max, format, &tm);
    xstack_ptr = XSTACK_SIZE - n;
    memmove(&xstack[xstack_ptr], xstack, n);
    return api_return_ax(n);
}
