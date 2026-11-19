/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The time of day on the RP6502-RIA: the always-on timer for its clock,
 * SET TZ for its zone, and SET LOC's language for its words. */

#include "core/tim/tim.h"
#include "core/tim/tz.h"
#include "ria/api/tim.h"
#include "core/sys/config.h"
#include "core/com/rln.h"
#include "core/str/str.h"
#include "ria/sys/cfg.h"
#include "machine.h"
#include <pico/aon_timer.h>
#include <stdio.h>
#include <string.h>

/* The zone of SET TZ, read from its TZ string when the setting changes. */
static tim_tz_t tim_tz;

void HOST_IN_FLASH_FUNC tim_init(void)
{
    // Noon UTC keeps localtime on 1 January 1970 for any TZ offset from UTC-12
    // to just under UTC+12.
    const struct timespec ts = {43200, 0};
    aon_timer_start(&ts);
    // cfg_init runs before tim_init, and loading the config file never calls
    // tim_apply_time_zone, so the stored zone is applied here.
    tim_apply_time_zone(tim_get_time_zone(), true);
}

bool tim_get_time(tim_spec_t *ts)
{
    struct timespec now;
    if (!aon_timer_get_time(&now))
        return false;
    ts->sec = now.tv_sec;
    ts->nsec = (uint32_t)now.tv_nsec;
    return true;
}

bool tim_set_time(const tim_spec_t *ts)
{
    const struct timespec to = {.tv_sec = ts->sec, .tv_nsec = (long)ts->nsec};
    return aon_timer_set_time(&to);
}

void tim_zone(int64_t utc, tim_zone_t *zone)
{
    tim_tz_zone(&tim_tz, utc, zone);
}

const char *tim_word(tim_word_t word)
{
    return S(STR_TIME_DAY_0 + word);
}

_Static_assert(STR_TIME_ABDAY_0 - STR_TIME_DAY_0 == TIM_WORD_ABDAY &&
               STR_TIME_MON_0 - STR_TIME_DAY_0 == TIM_WORD_MON &&
               STR_TIME_ABMON_0 - STR_TIME_DAY_0 == TIM_WORD_ABMON &&
               STR_TIME_AM - STR_TIME_DAY_0 == TIM_WORD_AM &&
               STR_TIME_T_FMT_AMPM - STR_TIME_DAY_0 == TIM_WORD_T_FMT_AMPM,
               "the STR_TIME_ strings are in the order of tim_word_t");

int HOST_IN_FLASH_FUNC tim_status_response(char *buf, size_t buf_size,
                                                                  int state, unsigned)
{
    (void)state;
    tim_spec_t ts = {0};
    tim_get_time(&ts);
    tim_tm_t tminfo;
    tim_localtime(ts.sec, &tminfo);
    tim_strftime(buf, buf_size, STR_STATUS_TIME, &tminfo);
    return -1;
}

int HOST_IN_FLASH_FUNC tim_tzdata_response(char *buf, size_t buf_size,
                                                                  int state, unsigned)
{
    if (state < 0)
        return state;
    size_t cell = 0;
    for (unsigned i = 0; i < tim_tz_count(); i++)
    {
        size_t len = strlen(tim_tz_name(i));
        if (len > cell)
            cell = len;
    }
    unsigned w = rln_get_term_width();
    if (w > buf_size - 2)
        w = buf_size - 2;
    unsigned cols = (w >= cell + 4) ? (w - 2) / (cell + 2) : 1;
    if (cols < 1)
        cols = 1;
    unsigned rows = (tim_tz_count() + cols - 1) / cols;
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
    for (unsigned i = 0; i < cols && el < tim_tz_count(); i++)
    {
        n = snprintf(buf, buf_size, "%-*s%*s",
                     (int)cell, tim_tz_name(el), (int)spread, "");
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

/* The config setter and loader copy the input into out before calling this,
 * so a name that is not in the table is stored as typed and is used as a
 * POSIX TZ string. One that is, is stored as the table has it, so "tokyo"
 * is "Asia/Tokyo". */
bool tim_check_time_zone(const char *in, char *out)
{
    const int i = tim_tz_find(in);
    if (i >= 0)
        strcpy(out, tim_tz_name((unsigned)i));
    return true;
}

/* The stored name is read instead of tz because the config setter passes tz
 * as it was typed. A string that is not a TZ string leaves the zone UTC. */
void tim_apply_time_zone(const char *tz, bool changed)
{
    (void)tz;
    (void)changed;
    const char *stored = tim_get_time_zone();
    const int i = tim_tz_find(stored);
    tim_tz_parse(i >= 0 ? tim_tz_string((unsigned)i) : stored, &tim_tz);
}

int HOST_IN_FLASH_FUNC tim_time_zone_response(char *buf, size_t buf_size,
                                                                        int state, unsigned width)
{
    (void)state;
    (void)width;
    snprintf(buf, buf_size, STR_SET_TZ_RESPONSE, tim_get_time_zone());
    return -1;
}
