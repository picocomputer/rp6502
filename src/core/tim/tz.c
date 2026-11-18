/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/tim/tz.h"
#include "machine.h"
#include <string.h>
#include <strings.h>

static bool tz_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool tz_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* A number of at most max. */
static bool tz_number(const char **s, int32_t max, int32_t *out)
{
    if (!tz_digit(**s))
        return false;
    int32_t v = 0;
    while (tz_digit(**s))
    {
        v = v * 10 + (*(*s)++ - '0');
        if (v > max)
            return false;
    }
    *out = v;
    return true;
}

/* A name: three letters or more, or anything but a > between < and >. */
static bool tz_name(const char **s, char *out)
{
    const char *p = *s;
    size_t n = 0;
    if (*p == '<')
    {
        for (p++; *p && *p != '>'; p++)
            if (n < TIM_TZ_NAME_MAX)
                out[n++] = *p;
        if (*p++ != '>')
            return false;
    }
    else
    {
        for (; tz_alpha(*p); p++)
            if (n < TIM_TZ_NAME_MAX)
                out[n++] = *p;
        if (p - *s < 3)
            return false;
    }
    out[n] = 0;
    *s = p;
    return n > 0;
}

/* [+-]hh[:mm[:ss]] as seconds. */
static bool tz_time(const char **s, int32_t max_hours, int32_t *out)
{
    bool negative = false;
    if (**s == '+' || **s == '-')
        negative = *(*s)++ == '-';
    int32_t h, m = 0, sec = 0;
    if (!tz_number(s, max_hours, &h))
        return false;
    if (**s == ':')
    {
        (*s)++;
        if (!tz_number(s, 59, &m))
            return false;
        if (**s == ':')
        {
            (*s)++;
            if (!tz_number(s, 59, &sec))
                return false;
        }
    }
    const int32_t v = h * 3600 + m * 60 + sec;
    *out = negative ? -v : v;
    return true;
}

static bool tz_rule(const char **s, tim_tz_rule_t *rule)
{
    int32_t v;
    memset(rule, 0, sizeof *rule);
    if (**s == 'M')
    {
        (*s)++;
        rule->kind = 'M';
        if (!tz_number(s, 12, &v) || v < 1 || *(*s)++ != '.')
            return false;
        rule->month = (uint8_t)v;
        if (!tz_number(s, 5, &v) || v < 1 || *(*s)++ != '.')
            return false;
        rule->week = (uint8_t)v;
        if (!tz_number(s, 6, &v))
            return false;
        rule->wday = (uint8_t)v;
    }
    else if (**s == 'J')
    {
        (*s)++;
        rule->kind = 'J';
        if (!tz_number(s, 365, &v) || v < 1)
            return false;
        rule->day = (uint16_t)v;
    }
    else
    {
        rule->kind = 'n';
        if (!tz_number(s, 365, &v))
            return false;
        rule->day = (uint16_t)v;
    }
    rule->time = 2 * 3600;
    if (**s == '/')
    {
        (*s)++;
        /* As far as a week either way, which POSIX has allowed since 2024
         * and the zones of Greenland need. */
        if (!tz_time(s, 167, &rule->time))
            return false;
    }
    return true;
}

static bool tz_parse(const char *s, tim_tz_t *tz)
{
    int32_t west;
    if (*s == ':')
        s++;
    if (!tz_name(&s, tz->std) || !tz_time(&s, 24, &west))
        return false;
    tz->std_offset = -west;
    if (!*s)
        return true;
    if (!tz_name(&s, tz->dst))
        return false;
    tz->has_dst = true;
    tz->dst_offset = tz->std_offset + 3600;
    if (*s && *s != ',')
    {
        if (!tz_time(&s, 24, &west))
            return false;
        tz->dst_offset = -west;
    }
    if (!*s)
        s = ",M3.2.0,M11.1.0";
    if (*s++ != ',' || !tz_rule(&s, &tz->start) ||
        *s++ != ',' || !tz_rule(&s, &tz->end))
        return false;
    return !*s;
}

bool tim_tz_parse(const char *s, tim_tz_t *tz)
{
    memset(tz, 0, sizeof *tz);
    if (tz_parse(s, tz))
        return true;
    memset(tz, 0, sizeof *tz);
    strcpy(tz->std, "UTC");
    return false;
}

/* When a rule falls in a year, in seconds since 1970 by the clock the rule
 * is in. */
static int64_t tz_rule_at(const tim_tz_rule_t *rule, int32_t year)
{
    const int32_t jan1 = tim_days_from_civil(year, 1, 1);
    const bool leap = tim_days_from_civil(year + 1, 1, 1) - jan1 == 366;
    int32_t days;
    if (rule->kind == 'M')
    {
        const int32_t first = tim_days_from_civil(year, rule->month, 1);
        const int32_t next = rule->month == 12 ? jan1 + 365 + leap
                                               : tim_days_from_civil(year, rule->month + 1, 1);
        /* The first d-day of the month, then whole weeks on, and the last
         * of them in the month where the week asked for is past it. */
        const int first_wday = (int)((first % 7 + 11) % 7);
        days = first + (rule->wday - first_wday + 7) % 7 + (rule->week - 1) * 7;
        if (days >= next)
            days -= 7;
    }
    else if (rule->kind == 'J')
        days = jan1 + rule->day - 1 + (leap && rule->day >= 60);
    else
        days = jan1 + rule->day;
    return (int64_t)days * 86400 + rule->time;
}

void tim_tz_zone(const tim_tz_t *tz, int64_t utc, tim_zone_t *zone)
{
    zone->offset = tz->std_offset;
    zone->dst = false;
    zone->name = tz->std;
    if (!tz->has_dst)
        return;
    /* The rules are in local time and are read for the year it is there.
     * South of the equator summer time starts later in the year than it
     * ends, and runs over the new year. */
    tim_tm_t date;
    if (!tim_gmtime(utc + tz->std_offset, &date))
        return;
    const int32_t year = date.tm_year + 1900;
    const int64_t start = tz_rule_at(&tz->start, year) - tz->std_offset;
    const int64_t end = tz_rule_at(&tz->end, year) - tz->dst_offset;
    if (start < end ? utc >= start && utc < end : utc >= start || utc < end)
    {
        zone->offset = tz->dst_offset;
        zone->dst = true;
        zone->name = tz->dst;
    }
}

/* ---- The zones that have a name ---- */

/* Each zone's name and then its TZ string, every one ended by its 0, and
 * the list by one more. */
static const char HOST_IN_FLASH tz_named[] =
#define X(name, tz) name "\0" tz "\0"
#include "core/def/tz.def"
#undef X
    ;

/* The n-th string of the list, or its end. */
static const char *tz_nth(unsigned n)
{
    const char *s = tz_named;
    while (n-- && *s)
        s += strlen(s) + 1;
    return s;
}

unsigned tim_tz_count(void)
{
    unsigned n = 0;
    for (const char *s = tz_named; *s; s += strlen(s) + 1)
        n++;
    return n / 2;
}

const char *tim_tz_name(unsigned i)
{
    return tz_nth(2 * i);
}

const char *tim_tz_string(unsigned i)
{
    return tz_nth(2 * i + 1);
}

int tim_tz_find(const char *name)
{
    int i = 0;
    for (const char *s = tz_named; *s; i++)
    {
        const char *slash = strchr(s, '/');
        if (!strcasecmp(name, s) || (slash && !strcasecmp(name, slash + 1)))
            return i;
        s += strlen(s) + 1;
        s += strlen(s) + 1;
    }
    return -1;
}
