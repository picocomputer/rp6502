/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MTIME is the soft CPU's microsecond counter. host_clock_us reads the high
 * word, the low word and the high word again, so a carry between the reads
 * never produces a torn value.
 *
 * Wall time comes from command 0x0090, which the host sends once at core
 * boot with the local time in seconds since 1970 and which sets RTC_VALID.
 * The command has no time zone field, so the UTC offset from the interact
 * menu converts that local reading to the UTC the API returns.
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

#define TIM_DEFAULT_EPOCH 43200

static int32_t tim_tz_min;
static int64_t tim_local_boot;
static int64_t tim_base_sec;
static uint64_t tim_base_us;

/* Only an offset is set for the zone, so the zone's name in the TZ string is
 * the offset in angle brackets, "<-0700>+7:00" for UTC-7. The name is written
 * east positive. The offset after it is west positive, because a POSIX offset
 * is the amount added to local time to get UTC, as in PST8 for UTC-8. */
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

void tim_stop(void) {}

/* A program cannot set the time, so recomputing tim_base_sec from
 * tim_local_boot when the offset changes discards nothing. */
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

/* MTIME stops only while the soft CPU is halted at its debug port for a
 * savestate, when no firmware runs, and a restore writes back the value it
 * had when the blob was made. */
uint64_t os_mono_ns(void)
{
    return host_clock_us() * 1000;
}

uint32_t os_random(void)
{
    return RTC_VALID ? (uint32_t)RTC_EPOCH : 0;
}

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
