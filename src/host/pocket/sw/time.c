/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MTIME is the machine's microsecond counter, which stands still while
 * the machine is held and which a savestate carries. UPTIME counts real
 * microseconds and nothing stops it. Each is read as the high word, the
 * low word and the high word again, so a carry between the reads never
 * produces a torn value.
 *
 * Wall time is the RTC reading plus UPTIME since boot, so it follows real
 * time through a save's hold and a load. The RTC reading comes from
 * command 0x0090, which the host sends once at core boot with the local
 * time in seconds since 1970 and which sets RTC_VALID. The command has no
 * time zone field, so the UTC offset from the interact menu converts that
 * local reading to the UTC the API returns.
 */

#include "core/sys/div64.h"
#include "mmio.h"

#include "tim.h"

#include "core/tim/tim.h"

#include "host/host.h"
#include "osal/os.h"


static uint64_t tim_read(volatile const uint32_t *lo_reg,
                         volatile const uint32_t *hi_reg)
{
    uint32_t hi, lo;
    do
    {
        hi = *hi_reg;
        lo = *lo_reg;
    } while (hi != *hi_reg);
    return ((uint64_t)hi << 32) | lo;
}

uint64_t host_clock_us(void)
{
    return tim_read(&MTIME_LO, &MTIME_HI);
}

static uint64_t tim_uptime_us(void)
{
    return tim_read(&UPTIME_LO, &UPTIME_HI);
}

#define TIM_DEFAULT_EPOCH 43200

static int32_t tim_tz_min;
static int64_t tim_local_boot;
static int64_t tim_base_sec;
static uint64_t tim_base_us;

/* Only an offset is set for the zone, so the zone's name is the offset,
 * east positive: "-0700" for UTC-7. */
static char tim_zone_name[6];

static void tim_apply_tz(void)
{
    const int32_t min = tim_tz_min < 0 ? -tim_tz_min : tim_tz_min;
    tim_zone_name[0] = tim_tz_min < 0 ? '-' : '+';
    tim_zone_name[1] = (char)('0' + min / 600 % 10);
    tim_zone_name[2] = (char)('0' + min / 60 % 10);
    tim_zone_name[3] = (char)('0' + min % 60 / 10);
    tim_zone_name[4] = (char)('0' + min % 10);
    tim_zone_name[5] = 0;
}

void tim_init(void)
{
    tim_local_boot = RTC_VALID ? (int64_t)RTC_EPOCH : TIM_DEFAULT_EPOCH;
    tim_tz_min = set_tz_minutes();
    tim_base_sec = tim_local_boot - (int64_t)tim_tz_min * 60;
    tim_base_us = tim_uptime_us();
    tim_apply_tz();
}

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

bool tim_get_time(tim_spec_t *ts)
{
    uint64_t us = tim_uptime_us() - tim_base_us;
    uint32_t into;
    ts->sec = tim_base_sec + (int64_t)div64(us, 1000000, &into);
    ts->nsec = into * 1000;
    return true;
}

bool tim_set_time(const tim_spec_t *ts)
{
    (void)ts;
    return false;
}

/* The Pocket's zone is one offset, the year round. */
void tim_zone(int64_t utc, tim_zone_t *zone)
{
    (void)utc;
    zone->offset = tim_tz_min * 60;
    zone->dst = false;
    zone->name = tim_zone_name;
}

/* The words of the C locale. The Pocket has no setting for its language,
 * and the strings of core/str would bring every one of them, in every
 * language, into its firmware. */
const char *tim_word(tim_word_t word)
{
    return tim_c_word(word);
}

/* A savestate's load puts mtime back where it stood at the save. The wall
 * time is this session's and does not move with it. */
void tim_clock_set(uint64_t us)
{
    MTIME_LO = (uint32_t)us;
    MTIME_HI = (uint32_t)(us >> 32);
}

uint64_t os_mono_ns(void)
{
    return host_clock_us() * 1000;
}

uint32_t os_random(void)
{
    return RTC_VALID ? (uint32_t)RTC_EPOCH : 0;
}

bool host_minimized(void)
{
    return false;
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
