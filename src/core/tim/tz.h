/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TIM_TZ_H_
#define _CORE_TIM_TZ_H_

/* A zone from a POSIX TZ string, for a machine with no operating system to
 * ask: "PST8PDT,M3.2.0/2,M11.1.0/2" is PST, 8 hours west, with PDT an hour
 * ahead of it from 2:00 on the second Sunday of March to 2:00 on the first
 * Sunday of November. */

#include "core/tim/tim.h"

/* When summer time starts or ends: Mm.w.d, the w-th d-day of month m with
 * 5 for the last; Jn, day n of 365 with February 29 never counted; or n, a
 * day from 0 with it counted. time is the seconds into that day, by the
 * clock in force before the change. */
typedef struct
{
    char kind; /* 'M', 'J' or 'n' */
    uint8_t month, week, wday;
    uint16_t day;
    int32_t time;
} tim_tz_rule_t;

#define TIM_TZ_NAME_MAX 10
typedef struct
{
    char std[TIM_TZ_NAME_MAX + 1];
    char dst[TIM_TZ_NAME_MAX + 1];
    int32_t std_offset; /* seconds east of UTC */
    int32_t dst_offset;
    bool has_dst;
    tim_tz_rule_t start, end;
} tim_tz_t;

/* Reads a TZ string. False, with tz left as UTC, for one that is not a TZ
 * string. A zone with summer time and no rules has those of the United
 * States, M3.2.0 and M11.1.0, as the C libraries give it. */
bool tim_tz_parse(const char *s, tim_tz_t *tz);

/* The zone at a time in UTC, for a machine's tim_zone. */
void tim_tz_zone(const tim_tz_t *tz, int64_t utc, tim_zone_t *zone);

/* The zones that have a name, such as "America/New_York", in core/def/tz.def: how
 * many, the name of each, and its TZ string. tim_tz_find is the number of
 * the zone a name is, in either case, with or without what is before its
 * slash, or -1. */
unsigned tim_tz_count(void);
const char *tim_tz_name(unsigned i);
const char *tim_tz_string(unsigned i);
int tim_tz_find(const char *name);

#endif /* _CORE_TIM_TZ_H_ */
