/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's clocks. The microsecond counter is the fabric's,
 * monotonic since power-on, read hi-lo-hi so a carry never shows a torn
 * value. Wall time starts from command 0x0090 at core boot: local time
 * as seconds since 1970, latched behind RTC_VALID. The Pocket knows
 * nothing of time zones, so the menu's UTC offset turns that local
 * reading into the UTC the API serves. DST is the user's job.
 *
 * The offset also becomes a POSIX TZ string, so localtime, mktime and
 * strftime's %z all agree. POSIX signs the offset westward, hence the
 * negation.
 *
 * A program cannot set it. The host owns this machine's time of day, and
 * there is nowhere to write one back to.
 */

#include "mmio.h"

#include "main.h"

#include "core/api/tim.h"

#include "host/host.h"
#include "osal/os.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint64_t host_clock_us(void)
{
    uint32_t hi, lo;
    do
    {
        hi = MTIME_HI;
        lo = MTIME_LO;
    } while (hi != MTIME_HI);
    return ((uint64_t)hi << 32) | lo;
}

/* Noon UTC keeps localtime on day 0 for any offset. */
#define TIM_DEFAULT_EPOCH 43200

static int32_t tim_tz_min;     /* minutes east of UTC, from the menu */
static int64_t tim_local_boot; /* the host's local reading at boot */
static int64_t tim_base_sec;   /* UTC at tim_base_us */
static uint64_t tim_base_us;

/* The zone has no name, so it is spelled as its own offset in POSIX's
 * bracketed form, "<-0700>+7:00". The two halves disagree on sign and
 * both are right: inside the brackets is a name, written east positive;
 * outside is the POSIX offset, which is what must be added to local time
 * to reach UTC.
 *
 * The brackets are also what makes it parse at all — a plain name must
 * be three characters or more, and the C library enforces that by giving
 * up silently and leaving TZ unset. */
static void tim_apply_tz(void)
{
    char tz[24];
    int32_t west = -tim_tz_min;
    snprintf(tz, sizeof tz, "<%+03ld%02ld>%+ld:%02ld",
             (long)(tim_tz_min / 60), labs((long)(tim_tz_min % 60)),
             (long)(west / 60), labs((long)(west % 60)));
    setenv("TZ", tz, 1);
    tzset();
}

void tim_init(void)
{
    tim_local_boot = RTC_VALID ? (int64_t)RTC_EPOCH : TIM_DEFAULT_EPOCH;
    tim_tz_min = set_tz_minutes();
    tim_base_sec = tim_local_boot - (int64_t)tim_tz_min * 60;
    tim_base_us = host_clock_us();
    tim_apply_tz();
}

/* The base is always the host's local reading, so a new offset re-derives
 * UTC from it. Nothing else can move the base: a program is refused. */
void tim_set_tz_minutes(int32_t min)
{
    if (min == tim_tz_min)
        return;
    tim_tz_min = min;
    tim_base_sec = tim_local_boot - (int64_t)min * 60;
    tim_apply_tz();
}

bool tim_get_time(struct timespec *ts)
{
    uint64_t us = host_clock_us() - tim_base_us;
    ts->tv_sec = tim_base_sec + (int64_t)(us / 1000000u);
    ts->tv_nsec = (long)(us % 1000000u) * 1000;
    return true;
}

/* The host wrote this machine's clock at boot and there is nowhere to write
 * one back. A program is refused rather than handed a base only this side
 * believes. */
bool tim_set_time(const struct timespec *ts)
{
    (void)ts;
    return false;
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

/* The fabric's mtime, which is also what host_clock_us reads. This machine has
 * one counter and answers both contracts from it. It stops only while the soft
 * CPU is halted at its debug port -- the savestate window -- where no code runs
 * to notice, and the engine jams the counter back on restore. */
uint64_t os_mono_ns(void)
{
    return host_clock_us() * 1000;
}

/* The wall clock the host wrote is this machine's only entropy. With no clock
 * the seed is zero, which is an ordinary seed -- so a bench that wants both
 * machines on one stream can pin the oracle to it. */
uint32_t os_random(void)
{
    return RTC_VALID ? (uint32_t)RTC_EPOCH : 0;
}

/* Nothing overrides a board -- no command line, no fixture -- so the seed is
 * the clock's, read once and held: the host can set the clock later, and a
 * seed that moved with it would fill from one and print another. */
static uint32_t seed;
static bool seed_taken;

uint32_t host_seed(void)
{
    if (!seed_taken)
    {
        seed = os_random();
        seed_taken = true;
    }
    return seed;
}
