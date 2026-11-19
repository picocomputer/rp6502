/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/tim/tim.h"
#include "core/str/oem.h"
#include "core/str/unicode.h"
#include "core/sys/div64.h"

/* Seconds as whole days and the seconds into the last, which is never
 * negative. For seconds of either sign up to SPLIT_MAX. */
#define DAY 86400
#define SPLIT_DAYS (1 << 30)
#define SPLIT_MAX ((int64_t)DAY * SPLIT_DAYS)
static int32_t tim_split(int64_t sec, int32_t *into_day)
{
    /* Made positive by a whole number of days, and divided with no call to
     * the compiler's 64-bit divide. */
    uint32_t rem;
    const uint64_t days = div64((uint64_t)(sec + SPLIT_MAX), DAY, &rem);
    *into_day = (int32_t)rem;
    return (int32_t)(uint32_t)days - SPLIT_DAYS;
}

/* Days to a date and back, by the 400 years the calendar repeats in, which
 * are 146097 days. The year starts in March, so the leap day is its last. */
int32_t tim_days_from_civil(int32_t year, int month, int day)
{
    year -= month <= 2;
    const int32_t era = (year >= 0 ? year : year - 399) / 400;
    const int32_t yoe = year - era * 400;
    const int32_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void tim_civil_from_days(int32_t days, int32_t *year, int *month, int *day)
{
    days += 719468;
    const int32_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int32_t doe = days - era * 146097;
    const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int32_t mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year = yoe + era * 400 + (*month <= 2);
}

bool tim_gmtime(int64_t t, tim_tm_t *out)
{
    if (t < -TIM_SEC_MAX || t > TIM_SEC_MAX)
        return false;
    int32_t sec;
    const int32_t days = tim_split(t, &sec);
    int32_t year;
    tim_civil_from_days(days, &year, &out->tm_mon, &out->tm_mday);
    out->tm_yday = (int)(days - tim_days_from_civil(year, 1, 1));
    out->tm_mon -= 1;
    out->tm_year = (int)(year - 1900);
    /* 1970-01-01 was a Thursday. */
    out->tm_wday = (int)((days % 7 + 11) % 7);
    out->tm_hour = (int)(sec / 3600);
    out->tm_min = (int)(sec / 60 % 60);
    out->tm_sec = (int)(sec % 60);
    out->tm_isdst = 0;
    out->tm_gmtoff = 0;
    out->tm_zone = "UTC";
    return true;
}

bool tim_localtime(int64_t t, tim_tm_t *out)
{
    if (t < -TIM_SEC_MAX || t > TIM_SEC_MAX)
        return false;
    tim_zone_t zone;
    tim_zone(t, &zone);
    if (!tim_gmtime(t + zone.offset, out))
        return false;
    out->tm_isdst = zone.dst;
    out->tm_gmtoff = zone.offset;
    out->tm_zone = zone.name;
    return true;
}

bool tim_timegm(const tim_tm_t *tm, int64_t *out)
{
    /* A field is an int, which a 6502 program gives 16 bits of, so the
     * seconds here are far inside what tim_split takes. */
    int32_t month = tm->tm_mon, year = tm->tm_year;
    if (year < -100000 || year > 100000 || month < -100000 || month > 100000)
        return false;
    year += 1900 + month / 12;
    month %= 12;
    if (month < 0)
        month += 12, year -= 1;
    const int64_t sec = (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec;
    if (sec < -SPLIT_MAX / 2 || sec > SPLIT_MAX / 2)
        return false;
    int32_t into_day;
    const int64_t days = (int64_t)tim_days_from_civil(year, (int)month + 1, 1) +
                         tm->tm_mday - 1 + tim_split(sec, &into_day);
    if (days < -TIM_DAYS_MAX || days > TIM_DAYS_MAX)
        return false;
    *out = (int64_t)(int32_t)days * DAY + into_day;
    return true;
}

bool tim_mktime(tim_tm_t *tm, int64_t *out)
{
    /* The date as if it were UTC is within a day of the answer, so the
     * zone's offset a day before and a day after are the two it can be
     * read with. An offset reads it right when the zone has that offset at
     * the time it gives. */
    int64_t naive;
    if (!tim_timegm(tm, &naive))
        return false;
    tim_zone_t before, after, at;
    tim_zone(naive - DAY, &before);
    tim_zone(naive + DAY, &after);
    tim_zone(naive - before.offset, &at);
    const bool before_ok = at.offset == before.offset;
    tim_zone(naive - after.offset, &at);
    const bool after_ok = at.offset == after.offset;
    int32_t offset;
    if (before.offset == after.offset || (before_ok && !after_ok))
        offset = before.offset;
    else if (after_ok && !before_ok)
        offset = after.offset;
    else
    {
        /* Both read it right: the clocks went back, and this time of day
         * came twice. Neither does: they went forward over it. Either way
         * tm_isdst picks summer time or standard time, and the larger
         * offset is summer time. */
        const bool summer = before.offset > after.offset;
        const int32_t summer_offset = summer ? before.offset : after.offset;
        const int32_t standard_offset = summer ? after.offset : before.offset;
        if (before_ok)
            offset = tm->tm_isdst == 0 ? standard_offset : summer_offset;
        else
            offset = tm->tm_isdst > 0 ? summer_offset : standard_offset;
    }
    const int64_t t = naive - offset;
    if (!tim_localtime(t, tm))
        return false;
    *out = t;
    return true;
}

void tim_tm_zone(tim_tm_t *tm)
{
    tim_tm_t date = *tm;
    int64_t t;
    if (tim_mktime(&date, &t))
    {
        tm->tm_gmtoff = date.tm_gmtoff;
        tm->tm_zone = date.tm_zone;
    }
    else
    {
        tm->tm_gmtoff = 0;
        tm->tm_zone = "";
    }
}

const char *tim_c_word(tim_word_t word)
{
    /* One after the other, each ended by its 0. */
    static const char words[] =
        "Sunday\0Monday\0Tuesday\0Wednesday\0Thursday\0Friday\0Saturday\0"
        "Sun\0Mon\0Tue\0Wed\0Thu\0Fri\0Sat\0"
        "January\0February\0March\0April\0May\0June\0"
        "July\0August\0September\0October\0November\0December\0"
        "Jan\0Feb\0Mar\0Apr\0May\0Jun\0Jul\0Aug\0Sep\0Oct\0Nov\0Dec\0"
        "AM\0PM\0"
        "%a %b %e %H:%M:%S %Y\0%m/%d/%y\0%H:%M:%S\0%I:%M:%S %p";
    const char *s = words;
    for (int i = 0; i < (int)word; i++)
        while (*s++)
            ;
    return s;
}

/* ---- strftime ---- */

typedef struct
{
    char *dst;
    size_t pos, max;
    const tim_tm_t *tm;
    char flag; /* of the conversion being made: -, _ or 0, or none */
} tim_out_t;

/* A result that fills max has no room for its 0, so it does not fit. */
static bool tim_put(tim_out_t *o, char ch)
{
    if (o->pos + 1 >= o->max)
        return false;
    o->dst[o->pos++] = ch;
    return true;
}

/* A number in at least width places, filled with pad, unless the
 * conversion's flag says to fill with something else or not at all. */
static bool tim_put_number(tim_out_t *o, int64_t value, int width, char pad)
{
    if (o->flag == '-')
        width = 1;
    else if (o->flag == '_')
        pad = ' ';
    else if (o->flag == '0')
        pad = '0';
    char digits[20];
    int n = 0;
    uint64_t v = value < 0 ? (uint64_t)-value : (uint64_t)value;
    do
    {
        uint32_t digit;
        v = div64(v, 10, &digit);
        digits[n++] = (char)('0' + digit);
    } while (v);
    if (value < 0 && !tim_put(o, '-'))
        return false;
    for (int i = n + (value < 0); i < width; i++)
        if (!tim_put(o, pad))
            return false;
    while (n)
        if (!tim_put(o, digits[--n]))
            return false;
    return true;
}

/* The next character of text in UTF-8, as the machine's words are, in the
 * code page. */
static unsigned char tim_next(const char **s)
{
    return unicode_from_utf8_next(s, oem_get_code_page_run());
}

static bool tim_put_utf8(tim_out_t *o, const char *s, bool lower)
{
    unsigned char ch;
    while ((ch = tim_next(&s)))
    {
        if (lower && ch >= 'A' && ch <= 'Z')
            ch = (unsigned char)(ch + ('a' - 'A'));
        if (!tim_put(o, (char)ch))
            return false;
    }
    return true;
}

/* The week of the year by ISO 8601, 1 to 53, and the year it belongs to:
 * weeks start on Monday, and the first is the one with January 4 in it. */
static int tim_iso_week(const tim_tm_t *tm, int32_t *iso_year)
{
    int32_t year = tm->tm_year + 1900;
    /* The day of the week of January 1, from Monday, and whether a year has
     * 53 weeks: it starts on a Thursday, or is a leap year that starts on
     * a Wednesday. */
    const int monday_based = (tm->tm_wday + 6) % 7;
    int week = (tm->tm_yday - monday_based + 10) / 7;
    for (int pass = 0; pass < 2; pass++)
    {
        const int32_t of = week < 1 ? year - 1 : year;
        const int32_t jan1 = tim_days_from_civil(of, 1, 1);
        const int first = (int)((jan1 % 7 + 10) % 7); /* from Monday */
        const bool leap = tim_days_from_civil(of + 1, 1, 1) - jan1 == 366;
        const int weeks = first == 3 || (leap && first == 2) ? 53 : 52;
        if (week < 1)
            week = weeks, year = of;
        else if (week > weeks)
            week = 1, year += 1;
        else
            break;
    }
    *iso_year = year;
    return week;
}

static int tim_floor_mod(int32_t v, int m)
{
    const int r = (int)(v % m);
    return r < 0 ? r + m : r;
}

static bool tim_format(tim_out_t *o, const char *format, bool utf8, int depth);

static bool tim_convert(tim_out_t *o, char conversion, int depth)
{
    const tim_tm_t *tm = o->tm;
    const int32_t year = tm->tm_year + 1900;
    int32_t iso_year;
    switch (conversion)
    {
    case 'a':
        return tim_put_utf8(o, tim_word(TIM_WORD_ABDAY + tim_floor_mod(tm->tm_wday, 7)), false);
    case 'A':
        return tim_put_utf8(o, tim_word(TIM_WORD_DAY + tim_floor_mod(tm->tm_wday, 7)), false);
    case 'b':
    case 'h':
        return tim_put_utf8(o, tim_word(TIM_WORD_ABMON + tim_floor_mod(tm->tm_mon, 12)), false);
    case 'B':
        return tim_put_utf8(o, tim_word(TIM_WORD_MON + tim_floor_mod(tm->tm_mon, 12)), false);
    case 'c':
        return depth < 2 && tim_format(o, tim_word(TIM_WORD_D_T_FMT), true, depth + 1);
    case 'C':
        return tim_put_number(o, (year - tim_floor_mod(year, 100)) / 100, 2, '0');
    case 'd':
        return tim_put_number(o, tm->tm_mday, 2, '0');
    case 'D':
        return tim_format(o, "%m/%d/%y", false, depth);
    case 'e':
        return tim_put_number(o, tm->tm_mday, 2, ' ');
    case 'F':
        return tim_format(o, "%Y-%m-%d", false, depth);
    case 'g':
        tim_iso_week(tm, &iso_year);
        return tim_put_number(o, tim_floor_mod(iso_year, 100), 2, '0');
    case 'G':
        tim_iso_week(tm, &iso_year);
        return tim_put_number(o, iso_year, 1, '0');
    case 'H':
        return tim_put_number(o, tm->tm_hour, 2, '0');
    case 'I':
        return tim_put_number(o, (tm->tm_hour + 11) % 12 + 1, 2, '0');
    case 'j':
        return tim_put_number(o, tm->tm_yday + 1, 3, '0');
    case 'k':
        return tim_put_number(o, tm->tm_hour, 2, ' ');
    case 'l':
        return tim_put_number(o, (tm->tm_hour + 11) % 12 + 1, 2, ' ');
    case 'm':
        return tim_put_number(o, tm->tm_mon + 1, 2, '0');
    case 'M':
        return tim_put_number(o, tm->tm_min, 2, '0');
    case 'n':
        return tim_put(o, '\n');
    case 'p':
    case 'P':
        return tim_put_utf8(o, tim_word(tm->tm_hour < 12 ? TIM_WORD_AM : TIM_WORD_PM),
                            conversion == 'P');
    case 'r':
        return depth < 2 && tim_format(o, tim_word(TIM_WORD_T_FMT_AMPM), true, depth + 1);
    case 'R':
        return tim_format(o, "%H:%M", false, depth);
    case 's':
    {
        tim_tm_t date = *tm;
        int64_t t;
        return tim_mktime(&date, &t) && tim_put_number(o, t, 1, '0');
    }
    case 'S':
        return tim_put_number(o, tm->tm_sec, 2, '0');
    case 't':
        return tim_put(o, '\t');
    case 'T':
        return tim_format(o, "%H:%M:%S", false, depth);
    case 'u':
        return tim_put_number(o, (tm->tm_wday + 6) % 7 + 1, 1, '0');
    case 'U':
        return tim_put_number(o, (tm->tm_yday + 7 - tm->tm_wday) / 7, 2, '0');
    case 'V':
        return tim_put_number(o, tim_iso_week(tm, &iso_year), 2, '0');
    case 'w':
        return tim_put_number(o, tm->tm_wday, 1, '0');
    case 'W':
        return tim_put_number(o, (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7, 2, '0');
    case 'x':
        return depth < 2 && tim_format(o, tim_word(TIM_WORD_D_FMT), true, depth + 1);
    case 'X':
        return depth < 2 && tim_format(o, tim_word(TIM_WORD_T_FMT), true, depth + 1);
    case 'y':
        return tim_put_number(o, tim_floor_mod(year, 100), 2, '0');
    case 'Y':
        return tim_put_number(o, year, 1, '0');
    case 'z':
    {
        const int32_t off = tm->tm_gmtoff < 0 ? -tm->tm_gmtoff : tm->tm_gmtoff;
        o->flag = 0;
        return tim_put(o, tm->tm_gmtoff < 0 ? '-' : '+') &&
               tim_put_number(o, off / 3600, 2, '0') &&
               tim_put_number(o, off / 60 % 60, 2, '0');
    }
    case 'Z':
        return tim_put_utf8(o, tm->tm_zone ? tm->tm_zone : "", false);
    case '%':
        return tim_put(o, '%');
    default:
        return tim_put(o, '%') && tim_put(o, conversion);
    }
}

/* A format a program gave is code page text. One of the machine's words is
 * UTF-8, and what it has between its conversions goes into the code page. */
static bool tim_format(tim_out_t *o, const char *format, bool utf8, int depth)
{
    while (*format)
    {
        if (*format != '%')
        {
            const char ch = utf8 ? (char)tim_next(&format) : *format++;
            if (!ch || !tim_put(o, ch))
                return false;
            continue;
        }
        format++;
        o->flag = 0;
        if (*format == '-' || *format == '_' || *format == '0')
            o->flag = *format++;
        if (*format == 'E' || *format == 'O')
            format++;
        if (!*format)
            return tim_put(o, '%');
        if (!tim_convert(o, *format++, depth))
            return false;
    }
    return true;
}

size_t tim_strftime(char *dst, size_t max, const char *format, const tim_tm_t *tm)
{
    if (!max)
        return 0;
    tim_out_t o = {dst, 0, max, tm, 0};
    if (!tim_format(&o, format, false, 0))
        o.pos = 0;
    dst[o.pos] = 0;
    return o.pos;
}
