/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* core/tim against answers that are known, on every host, and against the
 * C library where that is glibc, which reads the same POSIX TZ strings the
 * RP6502-RIA's SET TZ gives core/tim/tz.c. */

#include "core/tim/tim.h"
#include "core/tim/tz.h"
#include "utest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static tim_tm_t date(int year, int mon, int mday, int hour, int min, int sec)
{
    tim_tm_t tm = {0};
    tm.tm_year = year - 1900, tm.tm_mon = mon - 1, tm.tm_mday = mday;
    tm.tm_hour = hour, tm.tm_min = min, tm.tm_sec = sec;
    tm.tm_isdst = -1;
    return tm;
}

UTEST(tim, known_dates_in_utc)
{
    tim_tm_t tm;
    ASSERT_TRUE(tim_gmtime(0, &tm));
    ASSERT_EQ(tm.tm_year, 70);
    ASSERT_EQ(tm.tm_mon, 0);
    ASSERT_EQ(tm.tm_mday, 1);
    ASSERT_EQ(tm.tm_wday, 4);
    ASSERT_EQ(tm.tm_yday, 0);
    /* The manual's example: Sun Sep 16 01:03:52 1973. */
    ASSERT_TRUE(tim_gmtime(116989432, &tm));
    ASSERT_EQ(tm.tm_year, 73);
    ASSERT_EQ(tm.tm_mon, 8);
    ASSERT_EQ(tm.tm_mday, 16);
    ASSERT_EQ(tm.tm_hour, 1);
    ASSERT_EQ(tm.tm_min, 3);
    ASSERT_EQ(tm.tm_sec, 52);
    ASSERT_EQ(tm.tm_wday, 0);
    ASSERT_EQ(tm.tm_yday, 258);
    /* The second before the epoch, and a leap day. */
    ASSERT_TRUE(tim_gmtime(-1, &tm));
    ASSERT_EQ(tm.tm_year, 69);
    ASSERT_EQ(tm.tm_mon, 11);
    ASSERT_EQ(tm.tm_mday, 31);
    ASSERT_EQ(tm.tm_sec, 59);
    ASSERT_EQ(tm.tm_wday, 3);
    ASSERT_TRUE(tim_gmtime(951782400, &tm)); /* 2000-02-29 */
    ASSERT_EQ(tm.tm_mon, 1);
    ASSERT_EQ(tm.tm_mday, 29);
    ASSERT_EQ(tm.tm_yday, 59);
    ASSERT_FALSE(tim_gmtime(TIM_SEC_MAX + 1, &tm));
    ASSERT_FALSE(tim_gmtime(-TIM_SEC_MAX - 1, &tm));
}

UTEST(tim, a_date_out_of_range_carries)
{
    int64_t t;
    tim_tm_t tm = date(1973, 9, 16, 1, 3, 52);
    ASSERT_TRUE(tim_timegm(&tm, &t));
    ASSERT_EQ(t, (int64_t)116989432);
    /* Month 13 is January of the year after, day 0 the last of the month
     * before, and a negative second the minute before. */
    tm = date(1999, 13, 1, 0, 0, 0);
    ASSERT_TRUE(tim_timegm(&tm, &t));
    ASSERT_EQ(t, (int64_t)946684800);
    tm = date(2000, 3, 0, 24, 0, -1);
    ASSERT_TRUE(tim_timegm(&tm, &t));
    ASSERT_EQ(t, (int64_t)951868799);
    tm = date(2000, -11, 1, 0, 0, 0);
    ASSERT_TRUE(tim_timegm(&tm, &t));
    ASSERT_EQ(t, (int64_t)915148800); /* 1999-01-01 */
    tm = date(60000, 1, 1, 0, 0, 0);
    ASSERT_FALSE(tim_timegm(&tm, &t));
}

UTEST(tim, every_second_of_a_spread_goes_there_and_back)
{
    for (int64_t t = -TIM_SEC_MAX; t <= TIM_SEC_MAX; t += 86400ll * 3301 + 7919)
    {
        tim_tm_t tm;
        int64_t back;
        ASSERT_TRUE(tim_gmtime(t, &tm));
        ASSERT_TRUE(tim_timegm(&tm, &back));
        ASSERT_EQ(back, t);
    }
}

UTEST(tim, a_tz_string_is_read)
{
    tim_tz_t tz;
    ASSERT_TRUE(tim_tz_parse("PST8PDT,M3.2.0/2,M11.1.0/2", &tz));
    ASSERT_STREQ(tz.std, "PST");
    ASSERT_STREQ(tz.dst, "PDT");
    ASSERT_EQ(tz.std_offset, -8 * 3600);
    ASSERT_EQ(tz.dst_offset, -7 * 3600);
    ASSERT_TRUE(tz.has_dst);
    ASSERT_EQ(tz.start.month, 3);
    ASSERT_EQ(tz.start.week, 2);
    ASSERT_EQ(tz.end.month, 11);
    ASSERT_EQ(tz.end.time, 7200);
    ASSERT_TRUE(tim_tz_parse("NST3:30NDT,M3.2.0/2,M11.1.0/2", &tz));
    ASSERT_EQ(tz.std_offset, -(3 * 3600 + 1800));
    ASSERT_TRUE(tim_tz_parse("<-0700>+7:00", &tz));
    ASSERT_STREQ(tz.std, "-0700");
    ASSERT_EQ(tz.std_offset, -7 * 3600);
    ASSERT_FALSE(tz.has_dst);
    ASSERT_TRUE(tim_tz_parse("<+0530>-5:30", &tz));
    ASSERT_EQ(tz.std_offset, 5 * 3600 + 1800);
    ASSERT_TRUE(tim_tz_parse("EST5EDT", &tz));
    ASSERT_EQ(tz.start.month, 3);
    ASSERT_EQ(tz.end.month, 11);
    ASSERT_TRUE(tim_tz_parse("CET-1CEST,J60/2,300/3", &tz));
    ASSERT_EQ(tz.start.kind, 'J');
    ASSERT_EQ(tz.end.kind, 'n');
    ASSERT_EQ(tz.end.day, 300);
    /* Not a TZ string, and UTC answers. */
    ASSERT_FALSE(tim_tz_parse("Mars/Olympus", &tz));
    ASSERT_STREQ(tz.std, "UTC");
    ASSERT_EQ(tz.std_offset, 0);
    ASSERT_FALSE(tim_tz_parse("PST8PDT,M13.1.0,M11.1.0", &tz));
    ASSERT_FALSE(tim_tz_parse("", &tz));
    for (unsigned i = 0; i < tim_tz_count(); i++)
        ASSERT_TRUE(tim_tz_parse(tim_tz_string(i), &tz));
}

UTEST(tim, a_zone_is_found_by_its_name)
{
    const int tokyo = tim_tz_find("Asia/Tokyo");
    ASSERT_TRUE(tokyo >= 0);
    ASSERT_EQ(tim_tz_find("tokyo"), tokyo);
    ASSERT_EQ(tim_tz_find("ASIA/TOKYO"), tokyo);
    ASSERT_STREQ(tim_tz_name((unsigned)tokyo), "Asia/Tokyo");
    ASSERT_STREQ(tim_tz_string((unsigned)tokyo), "JST-9");
    ASSERT_EQ(tim_tz_find("Asia"), -1);
    ASSERT_EQ(tim_tz_find("JST-9"), -1);
}

UTEST(tim, summer_time_by_the_rules)
{
    tim_tz_t tz;
    tim_zone_t zone;
    ASSERT_TRUE(tim_tz_parse("PST8PDT,M3.2.0/2,M11.1.0/2", &tz));
    /* 2024-03-10 10:00 UTC is 02:00 PST, when the clocks go to 03:00 PDT. */
    tim_tz_zone(&tz, 1710064799, &zone);
    ASSERT_FALSE(zone.dst);
    ASSERT_EQ(zone.offset, -8 * 3600);
    ASSERT_STREQ(zone.name, "PST");
    tim_tz_zone(&tz, 1710064800, &zone);
    ASSERT_TRUE(zone.dst);
    ASSERT_EQ(zone.offset, -7 * 3600);
    ASSERT_STREQ(zone.name, "PDT");
    /* 2024-11-03 09:00 UTC is 02:00 PDT, when they go back to 01:00 PST. */
    tim_tz_zone(&tz, 1730624399, &zone);
    ASSERT_TRUE(zone.dst);
    tim_tz_zone(&tz, 1730624400, &zone);
    ASSERT_FALSE(zone.dst);
    /* South of the equator summer runs over the new year. */
    ASSERT_TRUE(tim_tz_parse("AEST-10AEDT,M10.1.0/2,M4.1.0/3", &tz));
    tim_tz_zone(&tz, 1704067200, &zone); /* 2024-01-01 */
    ASSERT_TRUE(zone.dst);
    ASSERT_EQ(zone.offset, 11 * 3600);
    tim_tz_zone(&tz, 1719792000, &zone); /* 2024-07-01 */
    ASSERT_FALSE(zone.dst);
}

/* strftime of a date in UTC, in the words of the C locale, which is what the
 * tests run in. */
static const char *format(const char *fmt, const tim_tm_t *tm)
{
    static char out[128];
    tim_strftime(out, sizeof out, fmt, tm);
    return out;
}

UTEST(tim, strftime_known)
{
    tim_tm_t tm;
    ASSERT_TRUE(tim_gmtime(116989432, &tm));
    ASSERT_STREQ(format("%a %b %e %H:%M:%S %Y", &tm), "Sun Sep 16 01:03:52 1973");
    ASSERT_STREQ(format("%A %B %d", &tm), "Sunday September 16");
    ASSERT_STREQ(format("%F %T %D %R", &tm), "1973-09-16 01:03:52 09/16/73 01:03");
    ASSERT_STREQ(format("%C %y %j %u %w %U %W %V %G %g", &tm), "19 73 259 7 0 37 37 37 1973 73");
    ASSERT_STREQ(format("%I %p %l %P %k", &tm), "01 AM  1 am  1");
    ASSERT_STREQ(format("%z %Z", &tm), "+0000 UTC");
    ASSERT_STREQ(format("%h%n%t%%", &tm), "Sep\n\t%");
    ASSERT_STREQ(format("%-d %_m %0e %-H|%Ey %Od", &tm), "16  9 16 1|73 16");
    ASSERT_STREQ(format("100%", &tm), "100%");
    ASSERT_STREQ(format("%Q", &tm), "%Q");
    /* Noon and midnight on a clock of 12 hours. */
    tm.tm_hour = 12;
    ASSERT_STREQ(format("%I %p", &tm), "12 PM");
    tm.tm_hour = 0;
    ASSERT_STREQ(format("%I %p", &tm), "12 AM");
    /* The first days of 2021 are in the last week of 2020 by ISO 8601, and
     * the last of 2024 in the first of 2025. */
    ASSERT_TRUE(tim_gmtime(1609545600, &tm)); /* 2021-01-02 */
    ASSERT_STREQ(format("%G %V %g", &tm), "2020 53 20");
    ASSERT_TRUE(tim_gmtime(1735603200, &tm)); /* 2024-12-31 */
    ASSERT_STREQ(format("%G %V", &tm), "2025 01");
}

UTEST(tim, strftime_that_does_not_fit_is_nothing)
{
    tim_tm_t tm;
    char out[8];
    ASSERT_TRUE(tim_gmtime(116989432, &tm));
    ASSERT_EQ(tim_strftime(out, 8, "%H:%M:%S", &tm), (size_t)0);
    ASSERT_STREQ(out, "");
    ASSERT_EQ(tim_strftime(out, 8, "%H:%M:%", &tm), (size_t)7);
    ASSERT_STREQ(out, "01:03:%");
    ASSERT_EQ(tim_strftime(out, 0, "%H", &tm), (size_t)0);
}

#if defined(__GLIBC__)

static void use_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

static bool same_date(const tim_tm_t *a, const struct tm *b)
{
    return a->tm_sec == b->tm_sec && a->tm_min == b->tm_min && a->tm_hour == b->tm_hour &&
           a->tm_mday == b->tm_mday && a->tm_mon == b->tm_mon && a->tm_year == b->tm_year &&
           a->tm_wday == b->tm_wday && a->tm_yday == b->tm_yday &&
           (a->tm_isdst > 0) == (b->tm_isdst > 0) && a->tm_gmtoff == b->tm_gmtoff;
}

/* Every half hour of four years, and a spread over two centuries. */
static int64_t next_time(int64_t t)
{
    static const int64_t years[][2] = {
        {915148800, 946684800},   /* 1999 */
        {1609459200, 1640995200}, /* 2021 */
        {1704067200, 1735689600}, /* 2024 */
        {2145916800, 2177452800}, /* 2038 */
    };
    for (size_t i = 0; i < sizeof years / sizeof years[0]; i++)
        if (t >= years[i][0] && t < years[i][1])
            return t + 1800;
    int64_t next = t + 86400 * 11 + 3607;
    for (size_t i = 0; i < sizeof years / sizeof years[0]; i++)
        if (t < years[i][0] && next >= years[i][0])
            return years[i][0];
    return next;
}
#define FIRST_TIME (-2208988800ll) /* 1900 */
#define LAST_TIME 4102444800ll     /* 2100 */

UTEST(tim, utc_as_glibc_has_it)
{
    for (int64_t t = FIRST_TIME; t < LAST_TIME; t = next_time(t))
    {
        time_t tt = (time_t)t;
        struct tm want;
        tim_tm_t got;
        gmtime_r(&tt, &want);
        ASSERT_TRUE(tim_gmtime(t, &got));
        ASSERT_TRUE(same_date(&got, &want));
    }
}

/* tz.c reads a TZ string as glibc does: the same offset, summer time and
 * name at every time, for every zone SET TZ has. */
UTEST(tim, every_zone_as_glibc_has_it)
{
    for (unsigned i = 0; i < tim_tz_count(); i++)
    {
        const char *string = tim_tz_string(i);
        tim_tz_t tz;
        ASSERT_TRUE(tim_tz_parse(string, &tz));
        use_tz(string);
        /* From 1970: glibc has no summer time before it for a TZ string,
         * where the rules here are every year's, as POSIX has them. */
        for (int64_t t = 0; t < LAST_TIME; t = next_time(t))
        {
            time_t tt = (time_t)t;
            struct tm want;
            tim_zone_t got;
            localtime_r(&tt, &want);
            tim_tz_zone(&tz, t, &got);
            if (got.offset != want.tm_gmtoff || got.dst != (want.tm_isdst > 0) ||
                strcmp(got.name, want.tm_zone))
            {
                fprintf(stderr, "%s at %lld: %d %d %s, glibc %ld %d %s\n", string,
                        (long long)t, got.offset, got.dst, got.name,
                        want.tm_gmtoff, want.tm_isdst, want.tm_zone);
                ASSERT_TRUE(false);
            }
        }
    }
}

/* The library with the host as its zone, which is glibc reading TZ: local
 * dates, and local dates back to seconds, where the clocks change too. */
static const char *const some_zones[] = {
    "UTC0",
    "EST5EDT,M3.2.0/2,M11.1.0/2",
    "GMT0BST,M3.5.0/1,M10.5.0/2",
    "AEST-10AEDT,M10.1.0/2,M4.1.0/3",
    "NST3:30NDT,M3.2.0/2,M11.1.0/2",
    "IST-5:30",
    "NZST-12NZDT,M9.5.0/2,M4.1.0/3",
    "LINT-14",
};

UTEST(tim, local_dates_as_glibc_has_them)
{
    for (size_t i = 0; i < sizeof some_zones / sizeof some_zones[0]; i++)
    {
        use_tz(some_zones[i]);
        for (int64_t t = FIRST_TIME; t < LAST_TIME; t = next_time(t))
        {
            time_t tt = (time_t)t;
            struct tm want;
            tim_tm_t got;
            localtime_r(&tt, &want);
            ASSERT_TRUE(tim_localtime(t, &got));
            ASSERT_TRUE(same_date(&got, &want));
            ASSERT_STREQ(got.tm_zone, want.tm_zone);
            /* And back, told which of two it is where the hour repeats. */
            int64_t back;
            tim_tm_t again = got;
            ASSERT_TRUE(tim_mktime(&again, &back));
            ASSERT_EQ(back, t);
        }
    }
}

UTEST(tim, mktime_as_glibc_has_it)
{
    for (size_t i = 0; i < sizeof some_zones / sizeof some_zones[0]; i++)
    {
        use_tz(some_zones[i]);
        /* Every half hour of 2024 by the clock on the wall, whether or not
         * the clock showed it, and not told which it is. */
        for (int half = -48; half < 367 * 48; half++)
        {
            struct tm want = {.tm_year = 124, .tm_mday = 1, .tm_min = half * 30, .tm_isdst = -1};
            tim_tm_t got = date(2024, 1, 1, 0, half * 30, 0);
            const time_t wt = mktime(&want);
            int64_t gt;
            ASSERT_TRUE(tim_mktime(&got, &gt));
            if (gt != (int64_t)wt)
            {
                /* An hour that came twice: glibc gives either, and this
                 * gives the first. Both must be that time of day. */
                struct tm other;
                time_t ot = (time_t)gt;
                localtime_r(&ot, &other);
                if (other.tm_hour != want.tm_hour || other.tm_min != want.tm_min ||
                    other.tm_mday != want.tm_mday || gt > (int64_t)wt)
                {
                    fprintf(stderr, "%s half %d: %lld, glibc %lld\n", some_zones[i], half,
                            (long long)gt, (long long)wt);
                    ASSERT_TRUE(false);
                }
            }
            else
                ASSERT_TRUE(same_date(&got, &want));
        }
    }
}

UTEST(tim, strftime_as_glibc_has_it)
{
    static const char conversions[] = "aAbBcCdDeFgGhHIjklmMnpPrRsStTuUVwWxXyYzZ%";
    for (size_t i = 0; i < sizeof some_zones / sizeof some_zones[0]; i++)
    {
        use_tz(some_zones[i]);
        for (int64_t t = FIRST_TIME; t < LAST_TIME; t += 86400ll * 53 + 3607 * 5)
        {
            time_t tt = (time_t)t;
            struct tm want;
            tim_tm_t got;
            localtime_r(&tt, &want);
            ASSERT_TRUE(tim_localtime(t, &got));
            for (const char *c = conversions; *c; c++)
            {
                char fmt[8], a[128], b[128];
                snprintf(fmt, sizeof fmt, "<%%%c>", *c);
                strftime(b, sizeof b, fmt, &want);
                tim_strftime(a, sizeof a, fmt, &got);
                if (strcmp(a, b))
                {
                    fprintf(stderr, "%s at %lld %s: \"%s\", glibc \"%s\"\n", some_zones[i],
                            (long long)t, fmt, a, b);
                    ASSERT_TRUE(false);
                }
            }
            static const char *const flagged[] = {"%-d|%_d|%0e", "%-H|%_M|%-m|%-I|%-j", "%_y|%-S"};
            for (size_t f = 0; f < sizeof flagged / sizeof flagged[0]; f++)
            {
                char a[128], b[128];
                strftime(b, sizeof b, flagged[f], &want);
                tim_strftime(a, sizeof a, flagged[f], &got);
                ASSERT_STREQ(a, b);
            }
        }
    }
}

#endif /* __GLIBC__ */

UTEST_MAIN();
