/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's clocks. The microsecond counter is the fabric's,
 * monotonic since reset and read hi-lo-hi so a carry between the two
 * words never shows a torn value. Wall time starts from the one thing
 * the host ever says about it: command 0x0090 at core boot, local
 * time as seconds since 1970, latched behind RTC_VALID. The Pocket has
 * no idea what a time zone is, so the menu's UTC offset — SET_TZ,
 * signed minutes east, persisted by the host like every menu value —
 * is what turns that local reading into the UTC the time API serves.
 * Moving the clock for DST is the user's job, exactly as the menu
 * says.
 *
 * The offset also becomes a POSIX TZ string handed to the C library,
 * so localtime, mktime and strftime's %z all agree through the one
 * mechanism the shared clk.c already uses on the RP2350. POSIX signs
 * the offset westward, hence the negation.
 *
 * A program that sets the clock owns it from then on: it set UTC, and
 * a later menu fiddle with the offset must not drag UTC around — the
 * offset only re-derives the base while the base is still the host's.
 */

#include "mmio.h"

#include "main.h"

#include "ria/api/tim.h"

#include <pico/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint64_t time_us_64(void)
{
    uint32_t hi, lo;
    do
    {
        hi = MTIME_HI;
        lo = MTIME_LO;
    } while (hi != MTIME_HI);
    return ((uint64_t)hi << 32) | lo;
}

/* Noon UTC keeps localtime on day 0 for any offset — the RP2350's own
 * convention for a clock nothing has set. */
#define TIM_DEFAULT_EPOCH 43200

static int32_t tim_tz_min;     /* minutes east of UTC, from the menu */
static int64_t tim_local_boot; /* the host's local reading at boot */
static int64_t tim_base_sec;   /* UTC at tim_base_us */
static int32_t tim_base_nsec;
static uint64_t tim_base_us;
static bool tim_program_set;   /* a program set UTC; the menu lets go */

/* Two things here read backwards and both are right.
 *
 * The offset's sign is inverted against the menu's. POSIX states an
 * offset as what must be *added to local time to reach UTC*, so a zone
 * seven hours east of Greenwich spells itself "-7". The tree's own
 * table says the same thing: CET is "CET-1", SAST is "SAST-2".
 *
 * And the name is not "UTC", because it is not UTC — it is whatever
 * zone the user set, and %Z prints it. The letters are arbitrary; only
 * their count matters.
 *
 * The zone's name must be three characters or more. POSIX says so, and
 * the C library enforces it by giving up without a word: a two-letter
 * name parses as no zone at all, TZ stays unset, and localtime quietly
 * returns UTC — which is exactly what "LT" did on hardware, both
 * clocks reading GMT while the offset itself was arriving fine. */
static void tim_apply_tz(void)
{
    char tz[16];
    int32_t west = -tim_tz_min;
    snprintf(tz, sizeof tz, "LOC%+ld:%02ld", (long)(west / 60),
             labs((long)(west % 60)));
    setenv("TZ", tz, 1);
    tzset();
}

void tim_init(void)
{
    tim_local_boot = RTC_VALID ? (int64_t)RTC_EPOCH : TIM_DEFAULT_EPOCH;
    tim_tz_min = (int32_t)SET_TZ;
    tim_base_sec = tim_local_boot - (int64_t)tim_tz_min * 60;
    tim_base_nsec = 0;
    tim_base_us = time_us_64();
    tim_apply_tz();
}

/* The menu moved the offset. UTC re-derives from the host's local
 * reading only while that reading is still the base; a clock a program
 * set is UTC already and stays put. */
void tim_set_tz_minutes(int32_t min)
{
    if (min == tim_tz_min)
        return;
    tim_tz_min = min;
    if (!tim_program_set)
        tim_base_sec = tim_local_boot - (int64_t)min * 60;
    tim_apply_tz();
}

bool tim_get_time(struct timespec *ts)
{
    uint64_t us = time_us_64() - tim_base_us;
    int64_t nsec = tim_base_nsec + (int64_t)(us % 1000000u) * 1000;
    ts->tv_sec = tim_base_sec + (int64_t)(us / 1000000u) + nsec / 1000000000;
    ts->tv_nsec = nsec % 1000000000;
    return true;
}

bool tim_set_time(const struct timespec *ts)
{
    tim_base_sec = ts->tv_sec;
    tim_base_nsec = (int32_t)ts->tv_nsec;
    tim_base_us = time_us_64();
    tim_program_set = true;
    return true;
}

void tim_get_time_res(struct timespec *ts)
{
    ts->tv_sec = 0;
    ts->tv_nsec = 1000;
}

bool tim_gmtime(time_t t, struct tm *out)
{
    return gmtime_r(&t, out) != NULL;
}

bool tim_localtime(time_t t, struct tm *out)
{
    return localtime_r(&t, out) != NULL;
}

size_t tim_strftime(char *dst, size_t max, const char *format,
                    const struct tm *tm)
{
    return strftime(dst, max, format, tm);
}
