/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// The original RP2040 RTC implementation by Brentward is here:
// https://github.com/picocomputer/rp6502/blob/bd8e3197/src/ria/api/clk.c

#include "api/api.h"
#include "api/clk.h"
#include "str/str.h"
#include "sys/cfg.h"
#include <hardware/timer.h>
#include <pico/aon_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_CLK)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#ifdef __INTELLISENSE__
#undef __in_flash
#define __in_flash(x)
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

static uint64_t clk_clock_start;
static int clk_tzinfo_index;

// Used with -Wl,--wrap=iswspace. Eliminates 26KB of Unicode/JIS tables.
int __wrap_iswspace(wint_t c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

int clk_tzdata_response(char *buf, size_t buf_size, int state)
{
    if (state < 0)
        return state;
    const char fmt[] = "   %-22s";
    unsigned rows = (CLK_TZINFO_COUNT + 2) / 3;
    unsigned el = state;
    for (int i = 0; i < 3; i++)
    {
        snprintf(buf, buf_size, fmt, clk_tzinfo_name[el]);
        buf += strlen(buf);
        if (i < 2)
            el += rows;
        else
            el += 1;
        if (el >= CLK_TZINFO_COUNT)
        {
            state = -2;
            break;
        }
    }
    *buf++ = '\n';
    *buf = 0;
    return state + 1;
}

void clk_init(void)
{
    // starting at noon avoids time zone wraparound
    const struct timespec ts = {43200, 0};
    aon_timer_start(&ts);
    // Default or finish loading
    if (clk_tzinfo_index >= 0)
    {
        setenv(STR_TZ, clk_tzinfo_tz[clk_tzinfo_index], 1);
        tzset();
    }
}

void clk_run(void)
{
    clk_clock_start = time_us_64();
}

int clk_status_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    struct timespec ts;
    if (!aon_timer_get_time(&ts))
    {
        snprintf(buf, buf_size, STR_STATUS_TIME, STR_INTERNAL_ERROR);
    }
    else
    {
        char tbuf[80];
        struct tm tminfo;
        localtime_r(&ts.tv_sec, &tminfo);
        strftime(tbuf, sizeof(tbuf), STR_STRFTIME, &tminfo);
        snprintf(buf, buf_size, STR_STATUS_TIME, tbuf);
    }
    return -1;
}

void clk_load_time_zone(const char *str, size_t len)
{
    char tz[CLK_TZ_MAX_SIZE];
    if (!str_parse_string(&str, &len, tz, sizeof(tz)))
        return;
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
    if (found_index != clk_tzinfo_index ||
        (found_index < 0 && clk_tzinfo_index < 0 &&
         strcmp(getenv(STR_TZ), tz)))
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

bool clk_api_tzset(void)
{
    struct __attribute__((packed))
    {
        int8_t daylight;
        int32_t timezone;
        char tzname[5];
        char dstname[5];
    } tz;
    tz.daylight = _daylight;
    tz.timezone = _timezone;
    strncpy(tz.tzname, tzname[0], 4);
    tz.tzname[4] = '\0';
    strncpy(tz.dstname, tzname[1], 4);
    tz.dstname[4] = '\0';
    for (size_t i = sizeof(tz); i;)
        if (!api_push_uint8(&(((uint8_t *)&tz)[--i])))
            return api_return_errno(API_EINVAL);
    return api_return_ax(0);
    static_assert(15 == sizeof(tz));
}

bool clk_api_tzquery(void)
{
    uint32_t requested_time = API_AXSREG;
    struct timespec ts;
    ts.tv_sec = requested_time;
    ts.tv_nsec = 0;
    struct tm local_tm = *localtime(&ts.tv_sec);
    struct tm gm_tm = *gmtime(&ts.tv_sec);
    gm_tm.tm_isdst = local_tm.tm_isdst;
    time_t local_sec = mktime(&local_tm);
    time_t gm_sec = mktime(&gm_tm);
    uint8_t isdst = local_tm.tm_isdst;
    api_push_uint8(&isdst);
    int32_t seconds = difftime(local_sec, gm_sec);
    return api_return_axsreg(seconds);
}

bool clk_api_clock(void)
{
    return api_return_axsreg((time_us_64() - clk_clock_start) / 10000);
}

bool clk_api_get_res(void)
{
    uint8_t clock_id = API_A;
    if (clock_id == CLK_ID_REALTIME)
    {
        struct timespec ts;
        aon_timer_get_resolution(&ts);
        int32_t nsec = ts.tv_nsec;
        uint32_t sec = ts.tv_sec;
        if (!api_push_int32(&nsec) ||
            !api_push_uint32(&sec))
            return api_return_errno(API_EINVAL);
        return api_return_ax(0);
    }
    else
        return api_return_errno(API_EINVAL);
}

bool clk_api_get_time(void)
{
    uint8_t clock_id = API_A;
    if (clock_id == CLK_ID_REALTIME)
    {
        struct timespec ts;
        aon_timer_get_time(&ts);
        int32_t nsec = ts.tv_nsec;
        uint32_t sec = ts.tv_sec;
        if (!api_push_int32(&nsec) ||
            !api_push_uint32(&sec))
            return api_return_errno(API_EINVAL);
        return api_return_ax(0);
    }
    else
        return api_return_errno(API_EINVAL);
}

bool clk_api_set_time(void)
{
    uint8_t clock_id = API_A;
    if (clock_id == CLK_ID_REALTIME)
    {
        uint32_t rawtime_sec;
        int32_t rawtime_nsec;
        if (!api_pop_uint32(&rawtime_sec) ||
            !api_pop_int32_end(&rawtime_nsec))
            return api_return_errno(API_EINVAL);
        struct timespec ts;
        ts.tv_sec = rawtime_sec;
        ts.tv_nsec = rawtime_nsec;
        if (!aon_timer_set_time(&ts))
            return api_return_errno(API_ERANGE);
        else
            return api_return_ax(0);
    }
    else
        return api_return_errno(API_EINVAL);
}
