/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TIM_TIM_H_
#define _CORE_TIM_TIM_H_

/* The time of day: seconds to a date and back, in UTC and in the machine's
 * zone, and strftime. It is the same code on every machine and calls
 * nothing of the C library's. A machine gives it the clock, the zone, and
 * the words, which are the functions it declares last and does not define.
 *
 * Beside it in this folder: clk.c is the 6502's calls for the time, tz.c is
 * a zone from a TZ string, for a machine with no operating system, and os.c
 * is the clock, zone, and words of a machine that has one. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* A time: seconds since 1970 in UTC, and nanoseconds into the second. */
typedef struct
{
    int64_t sec;
    uint32_t nsec;
} tim_spec_t;

/* A date and time of day, with the fields of C's struct tm under their
 * names: tm_mon counts from 0, tm_year from 1900, tm_wday from Sunday and
 * tm_yday from January 1. tm_gmtoff is the seconds this time is east of
 * UTC and tm_zone the zone's short name, which is the machine's to keep. */
typedef struct
{
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon;
    int tm_year, tm_wday, tm_yday, tm_isdst;
    int32_t tm_gmtoff;
    const char *tm_zone;
} tim_tm_t;

/* The times this converts: the years a 6502 program's struct tm can hold,
 * and some beyond. */
#define TIM_DAYS_MAX 12700000
#define TIM_SEC_MAX (86400ll * TIM_DAYS_MAX)

/* Seconds to a date in UTC, and to one in the machine's zone. False for a
 * time out of range. */
bool tim_gmtime(int64_t t, tim_tm_t *out);
bool tim_localtime(int64_t t, tim_tm_t *out);

/* A date to seconds, as C's mktime does it: only tm_sec through tm_year are
 * read, and one out of its range carries into the next, so month 12 is
 * January of the year after. tim_timegm reads the date as UTC. tim_mktime
 * reads it in the machine's zone. A time of day that summer time repeats is
 * the first of the two unless tm_isdst is 0, and one it skips is read in
 * the zone's standard time unless tm_isdst is over 0. False for a result out
 * of range. tim_mktime also makes tm the date of its result, every field. */
bool tim_timegm(const tim_tm_t *tm, int64_t *out);
bool tim_mktime(tim_tm_t *tm, int64_t *out);

/* Sets tm_gmtoff and tm_zone of a date in the machine's zone from its
 * fields and tm_isdst, for a date that came without them, as a 6502
 * program's does. */
void tim_tm_zone(tim_tm_t *tm);

/* C's strftime. The format is code page text and so is the result. It
 * returns the length of the result, which it ends with a 0, or 0 when the
 * result does not fit in max.
 *
 * The conversions are C99's: %a %A %b %B %c %C %d %D %e %F %g %G %h %H %I
 * %j %m %M %n %p %r %R %S %t %T %u %U %V %w %W %x %X %y %Y %z %Z %%. Four
 * more are not in C99: %k and %l are %H and %I padded with a space, %P is
 * %p in lower case, and %s is the seconds since 1970. Nor are three flags
 * for a conversion that is a number, which come right after the %: - for
 * no padding, _ to pad with spaces and 0 to pad with zeros, so %-d is the
 * day as 5 and %_H the hour as " 5". %E and %O before a conversion have
 * no effect. Anything else after a % is copied as it is. */
size_t tim_strftime(char *dst, size_t max, const char *format, const tim_tm_t *tm);

/* For tz.c: the days from 1970-01-01 to a date, where month is 1 to 12. */
int32_t tim_days_from_civil(int32_t year, int month, int day);

/* ---- What a machine gives ---- */

/* The clock: the time now, false when the machine does not know it; and a
 * new time for it, false on a machine whose clock a program may not set. */
bool tim_get_time(tim_spec_t *ts);
bool tim_set_time(const tim_spec_t *ts);

/* The zone at a time in UTC: how far east of UTC it is in seconds, whether
 * that is summer time, and its short name. */
typedef struct
{
    int32_t offset;
    bool dst;
    const char *name;
} tim_zone_t;
void tim_zone(int64_t utc, tim_zone_t *zone);

/* The words of the machine's language, in UTF-8: the days from Sunday and
 * the months from January, whole and short, AM and PM, and what %c, %x, %X
 * and %r stand for, each a format of its own. */
typedef enum
{
    TIM_WORD_DAY,                       /* and the six after it */
    TIM_WORD_ABDAY = TIM_WORD_DAY + 7,  /* and the six after it */
    TIM_WORD_MON = TIM_WORD_ABDAY + 7,  /* and the eleven after it */
    TIM_WORD_ABMON = TIM_WORD_MON + 12, /* and the eleven after it */
    TIM_WORD_AM = TIM_WORD_ABMON + 12,
    TIM_WORD_PM,
    TIM_WORD_D_T_FMT,
    TIM_WORD_D_FMT,
    TIM_WORD_T_FMT,
    TIM_WORD_T_FMT_AMPM,
    TIM_WORD_COUNT,
} tim_word_t;
const char *tim_word(tim_word_t word);

/* The words of C's own locale, which are English, for a machine with no
 * language of its own to give them in. */
const char *tim_c_word(tim_word_t word);

#endif /* _CORE_TIM_TIM_H_ */
