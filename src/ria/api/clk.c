/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// The original RP2040 RTC implementation by Brentward is here:
// https://github.com/picocomputer/rp6502/blob/bd8e3197/src/ria/api/clk.c

#include "api/api.h"
#include "api/clk.h"
#include "api/oem.h"
#include "str/rln.h"
#include "str/str.h"
#include "sys/cfg.h"
#include <hardware/timer.h>
#include <pico/aon_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_CLK)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define CLK_ID_REALTIME 0

#define CLK_TZINFO                                                         \
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
    static const char __in_flash("clk_tzinfo_strings") \
        CLK_TZINFO_NAME_##suffix[] = name;             \
    static const char __in_flash("clk_tzinfo_strings") \
        CLK_TZINFO_TZ_##suffix[] = tz;
CLK_TZINFO
#undef X

#define X(suffix, name, tz) \
    CLK_TZINFO_NAME_##suffix,
static const char *__in_flash("clk_tzinfo_name")
    clk_tzinfo_name[] = {CLK_TZINFO};
#undef X

#define X(suffix, name, tz) \
    CLK_TZINFO_TZ_##suffix,
static const char *__in_flash("clk_tzinfo_tz")
    clk_tzinfo_tz[] = {CLK_TZINFO};
#undef X

#define CLK_TZINFO_COUNT (sizeof(clk_tzinfo_name) / sizeof(*clk_tzinfo_name))

static uint64_t clk_start_us;
static int clk_tzinfo_index;

// Eliminates 26KB of Unicode/JIS tables brought in by tzset().
// Enabled with -Wl,--wrap=iswspace.
int __wrap_iswspace(wint_t c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

void __in_flash("clk_init") clk_init(void)
{
    // Noon UTC keeps localtime on day 0 for any TZ offset.
    const struct timespec ts = {43200, 0};
    aon_timer_start(&ts);
    // cfg_init ran first; apply any tz it loaded now that aon_timer is up.
    if (clk_tzinfo_index >= 0)
    {
        setenv(STR_TZ, clk_tzinfo_tz[clk_tzinfo_index], 1);
        tzset();
    }
}

void clk_run(void)
{
    clk_start_us = time_us_64();
}

// Locale-aware strftime emitting code page text. Conversions newlib would
// render in the C locale expand from the active locale, UTF-8 in flash
// converted to the active code page; the rest pass through newlib one spec
// at a time. Literal format bytes copy through verbatim.
static bool clk_strftime_worker(char *dst, size_t *pos, size_t max,
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
                !clk_strftime_worker(dst, pos, max, loc_format, tm, depth + 1))
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

static size_t clk_strftime(char *dst, size_t max, const char *format,
                           const struct tm *tm)
{
    size_t pos = 0;
    if (!max)
        return 0;
    if (!clk_strftime_worker(dst, &pos, max, format, tm, 0))
        pos = 0;
    dst[pos] = 0;
    return pos;
}

int clk_status_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    struct timespec ts;
    aon_timer_get_time(&ts);
    struct tm tminfo;
    localtime_r(&ts.tv_sec, &tminfo);
    clk_strftime(buf, buf_size, STR_STATUS_TIME, &tminfo);
    return -1;
}

int clk_tzdata_response(char *buf, size_t buf_size, int state, unsigned)
{
    if (state < 0)
        return state;
    size_t cell = 0;
    for (unsigned i = 0; i < CLK_TZINFO_COUNT; i++)
    {
        size_t len = strlen(clk_tzinfo_name[i]);
        if (len > cell)
            cell = len;
    }
    unsigned w = rln_get_term_width();
    if (w > buf_size - 2)
        w = buf_size - 2;
    unsigned cols = (w >= cell + 4) ? (w - 2) / (cell + 2) : 1;
    if (cols < 1)
        cols = 1;
    unsigned rows = (CLK_TZINFO_COUNT + cols - 1) / cols;
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
    for (unsigned i = 0; i < cols && el < CLK_TZINFO_COUNT; i++)
    {
        n = snprintf(buf, buf_size, "%-*s%*s",
                     (int)cell, clk_tzinfo_name[el], (int)spread, "");
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

void clk_load_time_zone(const char *str)
{
    char tz[CLK_TZ_MAX_SIZE];
    size_t n = strlen(str);
    if (n >= sizeof(tz))
        return;
    memcpy(tz, str, n);
    tz[n] = 0;
    for (unsigned i = 0; i < CLK_TZINFO_COUNT; i++)
    {
        if (!strcasecmp(tz, clk_tzinfo_name[i]))
        {
            clk_tzinfo_index = i;
            return;
        }
    }
    clk_tzinfo_index = -1;
    setenv(STR_TZ, tz, 1);
    tzset();
}

bool clk_set_time_zone(const char *tz)
{
    if (strlen(tz) >= CLK_TZ_MAX_SIZE)
        return false;
    int found_index = -1;
    for (unsigned i = 0; i < CLK_TZINFO_COUNT; i++)
    {
        const char *tzname = clk_tzinfo_name[i];
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
    if (found_index != clk_tzinfo_index ||
        (found_index < 0 && clk_tzinfo_index < 0 &&
         (!current_tz || strcmp(current_tz, tz))))
    {
        clk_tzinfo_index = found_index;
        if (clk_tzinfo_index < 0)
            setenv(STR_TZ, tz, 1);
        else
            setenv(STR_TZ, clk_tzinfo_tz[clk_tzinfo_index], 1);
        tzset();
        cfg_save();
    }
    return true;
}

const char *clk_get_time_zone(void)
{
    if (clk_tzinfo_index < 0)
        return getenv(STR_TZ);
    else
        return clk_tzinfo_name[clk_tzinfo_index];
}

uint32_t clk_get_run(uint32_t us_per_tick)
{
    return (time_us_64() - clk_start_us) / us_per_tick;
}

bool clk_api_clock(void)
{
    return api_return_axsreg(clk_get_run(10000));
}

bool clk_api_time_get(void)
{
    struct timespec ts;
    if (!aon_timer_get_time(&ts))
        return api_return_errno(API_EIO);
    int64_t sec = ts.tv_sec;
    if (!api_push_n(&sec, sizeof(sec)))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

bool clk_api_time_set(void)
{
    uint64_t u;
    if (!api_pop_uint64_end(&u))
        return api_return_errno(API_EINVAL);
    struct timespec ts = {.tv_sec = (int64_t)u, .tv_nsec = 0};
    if (!aon_timer_set_time(&ts))
        return api_return_errno(API_ERANGE);
    return api_return_ax(0);
}

struct __attribute__((packed)) clk_wire_tm
{
    int16_t tm_sec;
    int16_t tm_min;
    int16_t tm_hour;
    int16_t tm_mday;
    int16_t tm_mon;
    int16_t tm_year;
    int16_t tm_wday;
    int16_t tm_yday;
    int16_t tm_isdst;
};
static_assert(18 == sizeof(struct clk_wire_tm));

static void clk_tm_to_wire(const struct tm *tm, struct clk_wire_tm *w)
{
    w->tm_sec = tm->tm_sec;
    w->tm_min = tm->tm_min;
    w->tm_hour = tm->tm_hour;
    w->tm_mday = tm->tm_mday;
    w->tm_mon = tm->tm_mon;
    w->tm_year = tm->tm_year;
    w->tm_wday = tm->tm_wday;
    w->tm_yday = tm->tm_yday;
    w->tm_isdst = tm->tm_isdst;
}

static void clk_wire_to_tm(const struct clk_wire_tm *w, struct tm *tm)
{
    memset(tm, 0, sizeof(*tm));
    tm->tm_sec = w->tm_sec;
    tm->tm_min = w->tm_min;
    tm->tm_hour = w->tm_hour;
    tm->tm_mday = w->tm_mday;
    tm->tm_mon = w->tm_mon;
    tm->tm_year = w->tm_year;
    tm->tm_wday = w->tm_wday;
    tm->tm_yday = w->tm_yday;
    tm->tm_isdst = w->tm_isdst;
}

static bool clk_api_to_tm(bool local)
{
    uint64_t u;
    if (!api_pop_uint64_end(&u))
        return api_return_errno(API_EINVAL);
    // Short pushes are unsigned; 8-byte pushes carry the sign.
    time_t t = (int64_t)u;
    struct tm tm;
    if (!(local ? localtime_r(&t, &tm) : gmtime_r(&t, &tm)))
        return api_return_errno(API_EINVAL);
    if (tm.tm_year < INT16_MIN || tm.tm_year > INT16_MAX)
        return api_return_errno(API_ERANGE);
    struct clk_wire_tm w;
    clk_tm_to_wire(&tm, &w);
    if (!api_push_n(&w, sizeof(w)))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

bool clk_api_gmtime(void)
{
    return clk_api_to_tm(false);
}

bool clk_api_localtime(void)
{
    return clk_api_to_tm(true);
}

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

bool clk_api_strftime(void)
{
    const char *format = (char *)&xstack[xstack_ptr];
    // Compose below the format so they never overlap.
    size_t max = xstack_ptr;
    // The guard byte backstops a missing terminator. Validate before
    // advancing; core 1 serves 0xFFEC against xstack_ptr concurrently,
    // so it must never be published past XSTACK_SIZE.
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
    size_t n = clk_strftime((char *)xstack, max, format, &tm);
    // relocate buffer to top of xstack
    xstack_ptr = XSTACK_SIZE - n;
    memmove(&xstack[xstack_ptr], xstack, n);
    return api_return_ax(n);
}

// Deprecated. Retained for binaries built with older SDKs.

bool clk_api_tzset(void)
{
    struct __attribute__((packed))
    {
        int8_t daylight;
        int32_t timezone;
        char tzname[5];
        char dstname[5];
    } tz;
    static_assert(15 == sizeof(tz));
    tz.daylight = _daylight;
    tz.timezone = _timezone;
    strncpy(tz.tzname, tzname[0], 4);
    tz.tzname[4] = '\0';
    strncpy(tz.dstname, tzname[1], 4);
    tz.dstname[4] = '\0';
    if (!api_push_n(&tz, sizeof(tz)))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

bool clk_api_tzquery(void)
{
    uint32_t epoch_sec = API_AXSREG;
    struct timespec ts;
    ts.tv_sec = epoch_sec;
    ts.tv_nsec = 0;
    // mktime(localtime(t)) - mktime(gmtime(t) with local DST) yields the
    // UTC offset in seconds east of UTC.
    struct tm local_tm = *localtime(&ts.tv_sec);
    struct tm gm_tm = *gmtime(&ts.tv_sec);
    gm_tm.tm_isdst = local_tm.tm_isdst;
    time_t local_sec = mktime(&local_tm);
    time_t gm_sec = mktime(&gm_tm);
    uint8_t isdst = local_tm.tm_isdst;
    if (!api_push_uint8(&isdst))
        return api_return_errno(API_EINVAL);
    int32_t seconds = (int32_t)(local_sec - gm_sec);
    return api_return_axsreg(seconds);
}

bool clk_api_get_res(void)
{
    if (API_A != CLK_ID_REALTIME)
        return api_return_errno(API_EINVAL);
    struct timespec ts;
    aon_timer_get_resolution(&ts);
    int32_t nsec = ts.tv_nsec;
    uint32_t sec = ts.tv_sec;
    if (!api_push_int32(&nsec) ||
        !api_push_uint32(&sec))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

bool clk_api_get_time(void)
{
    if (API_A != CLK_ID_REALTIME)
        return api_return_errno(API_EINVAL);
    struct timespec ts;
    if (!aon_timer_get_time(&ts))
        return api_return_errno(API_EIO);
    int32_t nsec = ts.tv_nsec;
    uint32_t sec = ts.tv_sec;
    if (!api_push_int32(&nsec) ||
        !api_push_uint32(&sec))
        return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

bool clk_api_set_time(void)
{
    if (API_A != CLK_ID_REALTIME)
        return api_return_errno(API_EINVAL);
    uint32_t rawtime_sec;
    int32_t rawtime_nsec;
    if (!api_pop_uint32(&rawtime_sec) ||
        !api_pop_int32_end(&rawtime_nsec))
        return api_return_errno(API_EINVAL);
    if (rawtime_nsec < 0 || rawtime_nsec > 999999999)
        return api_return_errno(API_EINVAL);
    struct timespec ts;
    ts.tv_sec = rawtime_sec;
    ts.tv_nsec = rawtime_nsec;
    if (!aon_timer_set_time(&ts))
        return api_return_errno(API_ERANGE);
    return api_return_ax(0);
}
