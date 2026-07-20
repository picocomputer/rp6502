/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/api/oem.h"
#include "ria/api/tim.h"
#include "ria/str/rln.h"
#include "ria/str/str.h"
#include "ria/sys/cfg.h"
#include <pico/aon_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define TIM_TZINFO                                                         \
    X(UTC, "Etc/UTC", "UTC0")                                              \
    X(GMT0GH, "Africa/Accra", "GMT0")                                      \
    X(CETn1DZ, "Africa/Algiers", "CET-1")                                  \
    X(EETn2EG, "Africa/Cairo", "EET-2")                                    \
    X(WETn1MA, "Africa/Casablanca", "WET-1")                               \
    X(SASTn2ZA, "Africa/Johannesburg", "SAST-2")                           \
    X(WATn1NG, "Africa/Lagos", "WAT-1")                                    \
    X(EATn3KE, "Africa/Nairobi", "EAT-3")                                  \
    X(AKST9, "America/Anchorage", "AKST9AKDT,M3.2.0/2,M11.1.0/2")          \
    X(COT5CO, "America/Bogota", "COT5")                                    \
    X(ART3AR, "America/Buenos_Aires", "ART3")                              \
    X(VET4VE, "America/Caracas", "VET4")                                   \
    X(CST6, "America/Chicago", "CST6CDT,M3.2.0/2,M11.1.0/2")               \
    X(MST7, "America/Denver", "MST7MDT,M3.2.0/2,M11.1.0/2")                \
    X(MST7CA, "America/Edmonton", "MST7MDT,M3.2.0/2,M11.1.0/2")            \
    X(AST4CA, "America/Halifax", "AST4ADT,M3.2.0/2,M11.1.0/2")             \
    X(PET5PE, "America/Lima", "PET5")                                      \
    X(PST8, "America/Los_Angeles", "PST8PDT,M3.2.0/2,M11.1.0/2")           \
    X(CST6MX, "America/Mexico_City", "CST6")                               \
    X(UYT3UY, "America/Montevideo", "UYT3")                                \
    X(FNT2BR, "America/Noronha", "FNT2")                                   \
    X(EST5, "America/New_York", "EST5EDT,M3.2.0/2,M11.1.0/2")              \
    X(EST5PA, "America/Panama", "EST5")                                    \
    X(MST7AZ, "America/Phoenix", "MST7")                                   \
    X(BRT3BR, "America/Sao_Paulo", "BRT3")                                 \
    X(NST3CA, "America/St_Johns", "NST3:30NDT,M3.2.0/2,M11.1.0/2")         \
    X(EST5CA, "America/Toronto", "EST5EDT,M3.2.0/2,M11.1.0/2")             \
    X(PST8CA, "America/Vancouver", "PST8PDT,M3.2.0/2,M11.1.0/2")           \
    X(CST6CA, "America/Winnipeg", "CST6CDT,M3.2.0/2,M11.1.0/2")            \
    X(ICTn7, "Asia/Bangkok", "ICT-7")                                      \
    X(BDTn6, "Asia/Dhaka", "BDT-6")                                        \
    X(GSTn4, "Asia/Dubai", "GST-4")                                        \
    X(HKTn8, "Asia/Hong_Kong", "HKT-8")                                    \
    X(WIBn7ID, "Asia/Jakarta", "WIB-7")                                    \
    X(ISTn2IL, "Asia/Jerusalem", "IST-2IDT,M3.5.0/2,M10.5.0/2")            \
    X(AFTn4, "Asia/Kabul", "AFT-4:30")                                     \
    X(PKTn5, "Asia/Karachi", "PKT-5")                                      \
    X(NPTn5, "Asia/Kathmandu", "NPT-5:45")                                 \
    X(ISTn5, "Asia/Kolkata", "IST-5:30")                                   \
    X(PHTn8PH, "Asia/Manila", "PHT-8")                                     \
    X(ASTn3SA, "Asia/Riyadh", "AST-3")                                     \
    X(CSTn8, "Asia/Shanghai", "CST-8")                                     \
    X(KSTn9, "Asia/Seoul", "KST-9")                                        \
    X(SGTn8, "Asia/Singapore", "SGT-8")                                    \
    X(IRSTn3, "Asia/Tehran", "IRST-3:30")                                  \
    X(JSTn9, "Asia/Tokyo", "JST-9")                                        \
    X(MMTn6, "Asia/Yangon", "MMT-6:30")                                    \
    X(ACSTn9dst, "Australia/Adelaide", "ACST-9:30ACDT,M10.1.0/2,M4.1.0/3") \
    X(AESTn10, "Australia/Brisbane", "AEST-10")                            \
    X(ACSTn9, "Australia/Darwin", "ACST-9:30")                             \
    X(AWSTn8, "Australia/Perth", "AWST-8")                                 \
    X(AESTn10dst, "Australia/Sydney", "AEST-10AEDT,M10.1.0/2,M4.1.0/3")    \
    X(CETn1, "Europe/Berlin", "CET-1CEST,M3.5.0/2,M10.5.0/3")              \
    X(EETn2, "Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4")            \
    X(TRTn3, "Europe/Istanbul", "TRT-3")                                   \
    X(WET0, "Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0/2")                \
    X(GMT0, "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0/2")                 \
    X(MSKn3, "Europe/Moscow", "MSK-3")                                     \
    X(CETn1FR, "Europe/Paris", "CET-1CEST,M3.5.0/2,M10.5.0/3")             \
    X(NZSTn12, "Pacific/Auckland", "NZST-12NZDT,M9.5.0/2,M4.1.0/3")        \
    X(WSTn13, "Pacific/Apia", "WST-13")                                    \
    X(ChSTn10, "Pacific/Guam", "ChST-10")                                  \
    X(HST10, "Pacific/Honolulu", "HST10")                                  \
    X(LINTn14, "Pacific/Kiritimati", "LINT-14")                            \
    X(NCTn11, "Pacific/Noumea", "NCT-11")                                  \
    X(SST11, "Pacific/Pago_Pago", "SST11")

#define X(suffix, name, tz)                            \
    static const char __in_flash("tim_tzinfo_strings") \
        TIM_TZINFO_NAME_##suffix[] = name;             \
    static const char __in_flash("tim_tzinfo_strings") \
        TIM_TZINFO_TZ_##suffix[] = tz;
TIM_TZINFO
#undef X

#define X(suffix, name, tz) \
    TIM_TZINFO_NAME_##suffix,
static const char *__in_flash("tim_tzinfo_name")
    tim_tzinfo_name[] = {TIM_TZINFO};
#undef X

#define X(suffix, name, tz) \
    TIM_TZINFO_TZ_##suffix,
static const char *__in_flash("tim_tzinfo_tz")
    tim_tzinfo_tz[] = {TIM_TZINFO};
#undef X

#define TIM_TZINFO_COUNT (sizeof(tim_tzinfo_name) / sizeof(*tim_tzinfo_name))

static int tim_tzinfo_index;

// Eliminates 26KB of Unicode/JIS tables brought in by tzset().
// Enabled with -Wl,--wrap=iswspace.
int __wrap_iswspace(wint_t c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

void __in_flash("tim_init") tim_init(void)
{
    // Noon UTC keeps localtime on day 0 for any TZ offset.
    const struct timespec ts = {43200, 0};
    aon_timer_start(&ts);
    // cfg_init ran first; apply any tz it loaded now that aon_timer is up.
    if (tim_tzinfo_index >= 0)
    {
        setenv(STR_TZ, tim_tzinfo_tz[tim_tzinfo_index], 1);
        tzset();
    }
}

bool tim_get_time(struct timespec *ts)
{
    return aon_timer_get_time(ts);
}

bool tim_set_time(const struct timespec *ts)
{
    return aon_timer_set_time(ts);
}

void tim_get_time_res(struct timespec *ts)
{
    aon_timer_get_resolution(ts);
}

bool tim_localtime(time_t t, struct tm *out)
{
    return localtime_r(&t, out) != NULL;
}

bool tim_gmtime(time_t t, struct tm *out)
{
    return gmtime_r(&t, out) != NULL;
}

int tim_get_tz_daylight(void)
{
    return _daylight;
}

long tim_get_tz_offset(void)
{
    return _timezone;
}

const char *tim_get_tz_name(bool dst)
{
    return tzname[dst];
}

// Locale-aware strftime emitting code page text. Conversions newlib would
// render in the C locale expand from the active locale, UTF-8 in flash
// converted to the active code page; the rest pass through newlib one spec
// at a time. Literal format bytes copy through verbatim.
static bool tim_strftime_worker(char *dst, size_t *pos, size_t max,
                                const char *format, const struct tm *tm,
                                int depth)
{
    while (*format)
    {
        if (*format != '%')
        {
            if (*pos + 1 >= max)
                return false;
            dst[(*pos)++] = *format++;
            continue;
        }
        format++;
        // Era and alternate-digit forms are not supported.
        if (*format == 'E' || *format == 'O')
            format++;
        char conversion = *format;
        if (!conversion)
            break;
        format++;
        const char *loc = NULL;
        const char *loc_format = NULL;
        switch (conversion)
        {
        case 'a':
            loc = S(STR_TIME_ABDAY_0 + tm->tm_wday);
            break;
        case 'A':
            loc = S(STR_TIME_DAY_0 + tm->tm_wday);
            break;
        case 'b':
        case 'h':
            loc = S(STR_TIME_ABMON_0 + tm->tm_mon);
            break;
        case 'B':
            loc = S(STR_TIME_MON_0 + tm->tm_mon);
            break;
        case 'p':
            loc = S(tm->tm_hour < 12 ? STR_TIME_AM : STR_TIME_PM);
            break;
        case 'c':
            loc_format = S(STR_TIME_D_T_FMT);
            break;
        case 'x':
            loc_format = S(STR_TIME_D_FMT);
            break;
        case 'X':
            loc_format = S(STR_TIME_T_FMT);
            break;
        case 'r':
            loc_format = S(STR_TIME_T_FMT_AMPM);
            break;
        }
        if (loc)
        {
            unsigned char ch;
            while ((ch = oem_from_utf8_next(&loc)))
            {
                if (*pos + 1 >= max)
                    return false;
                dst[(*pos)++] = ch;
            }
        }
        else if (loc_format)
        {
            if (depth >= 2 ||
                !tim_strftime_worker(dst, pos, max, loc_format, tm, depth + 1))
                return false;
        }
        else
        {
            char spec[3] = {'%', conversion, 0};
            char tmp[32];
            size_t n = strftime(tmp, sizeof(tmp), spec, tm);
            if (*pos + n >= max)
                return false;
            memcpy(&dst[*pos], tmp, n);
            *pos += n;
        }
    }
    return true;
}

size_t tim_strftime(char *dst, size_t max, const char *format,
                    const struct tm *tm)
{
    size_t pos = 0;
    if (!max)
        return 0;
    if (!tim_strftime_worker(dst, &pos, max, format, tm, 0))
        pos = 0;
    dst[pos] = 0;
    return pos;
}

int tim_status_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    struct timespec ts;
    aon_timer_get_time(&ts);
    struct tm tminfo;
    localtime_r(&ts.tv_sec, &tminfo);
    tim_strftime(buf, buf_size, STR_STATUS_TIME, &tminfo);
    return -1;
}

int tim_tzdata_response(char *buf, size_t buf_size, int state, unsigned)
{
    if (state < 0)
        return state;
    size_t cell = 0;
    for (unsigned i = 0; i < TIM_TZINFO_COUNT; i++)
    {
        size_t len = strlen(tim_tzinfo_name[i]);
        if (len > cell)
            cell = len;
    }
    unsigned w = rln_get_term_width();
    if (w > buf_size - 2)
        w = buf_size - 2;
    unsigned cols = (w >= cell + 4) ? (w - 2) / (cell + 2) : 1;
    if (cols < 1)
        cols = 1;
    unsigned rows = (TIM_TZINFO_COUNT + cols - 1) / cols;
    if ((unsigned)state >= rows)
        return -1;
    unsigned spread = (w - cols * cell) / (cols + 1);
    int n = snprintf(buf, buf_size, "%*s", (int)spread, "");
    if (n > 0 && (size_t)n < buf_size)
    {
        buf += n;
        buf_size -= n;
    }
    unsigned el = state;
    for (unsigned i = 0; i < cols && el < TIM_TZINFO_COUNT; i++)
    {
        n = snprintf(buf, buf_size, "%-*s%*s",
                     (int)cell, tim_tzinfo_name[el], (int)spread, "");
        if (n > 0 && (size_t)n < buf_size)
        {
            buf += n;
            buf_size -= n;
        }
        el += rows;
    }
    snprintf(buf, buf_size, "\n");
    return ((unsigned)state + 1 < rows) ? state + 1 : -1;
}

void tim_load_time_zone(const char *str)
{
    char tz[TIM_TZ_MAX_SIZE];
    size_t n = strlen(str);
    if (n >= sizeof(tz))
        return;
    memcpy(tz, str, n);
    tz[n] = 0;
    for (unsigned i = 0; i < TIM_TZINFO_COUNT; i++)
    {
        if (!strcasecmp(tz, tim_tzinfo_name[i]))
        {
            tim_tzinfo_index = i;
            return;
        }
    }
    tim_tzinfo_index = -1;
    setenv(STR_TZ, tz, 1);
    tzset();
}

bool tim_set_time_zone(const char *tz)
{
    if (strlen(tz) >= TIM_TZ_MAX_SIZE)
        return false;
    int found_index = -1;
    for (unsigned i = 0; i < TIM_TZINFO_COUNT; i++)
    {
        const char *tzname = tim_tzinfo_name[i];
        if (!strcasecmp(tz, tzname))
        {
            found_index = i;
            break;
        }
        const char *slash = strchr(tzname, '/');
        if (slash && !strcasecmp(tz, slash + 1))
        {
            found_index = i;
            break;
        }
    }
    const char *current_tz = getenv(STR_TZ);
    if (found_index != tim_tzinfo_index ||
        (found_index < 0 && tim_tzinfo_index < 0 &&
         (!current_tz || strcmp(current_tz, tz))))
    {
        tim_tzinfo_index = found_index;
        if (tim_tzinfo_index < 0)
            setenv(STR_TZ, tz, 1);
        else
            setenv(STR_TZ, tim_tzinfo_tz[tim_tzinfo_index], 1);
        tzset();
        cfg_save();
    }
    return true;
}

const char *tim_get_time_zone(void)
{
    if (tim_tzinfo_index < 0)
        return getenv(STR_TZ);
    else
        return tim_tzinfo_name[tim_tzinfo_index];
}
