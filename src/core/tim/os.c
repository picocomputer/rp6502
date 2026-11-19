/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/tim/os.h"
#include "core/tim/tim.h"
#include "osal/os.h"
#include <string.h>
#include <time.h>

void tim_init(void)
{
    os_locale_reset();
}

void tim_stop(void)
{
    os_locale_free();
}

bool tim_get_time(tim_spec_t *ts)
{
    ts->sec = time(NULL);
    ts->nsec = 0;
    return true;
}

/* The time is read from the host's clock, so a program that asks to move it
 * is refused rather than given an offset only this machine would apply. */
bool tim_set_time(const tim_spec_t *ts)
{
    (void)ts;
    return false;
}

/* A date keeps the name of its zone, so the names are kept here: one for
 * standard time and one for summer time, as the host last gave them. */
#define TIM_ZONE_NAME_SIZE 48
static char tim_zone_name[2][TIM_ZONE_NAME_SIZE];

void tim_zone(int64_t utc, tim_zone_t *zone)
{
    char name[TIM_ZONE_NAME_SIZE];
    os_zone(utc, &zone->offset, &zone->dst, name, sizeof name);
    char *kept = tim_zone_name[zone->dst];
    if (strcmp(kept, name))
        strcpy(kept, name);
    zone->name = kept;
}

/* The host's word, or the C locale's where the host has none. A host with no AM and PM has them empty, and those stay
 * empty. A format that is empty is one the locale does not have, as %r is
 * where the clock has 24 hours. */
const char *tim_word(tim_word_t word)
{
    const char *s = os_time_word(word);
    if (!s || (word >= TIM_WORD_D_T_FMT && !*s))
        s = tim_c_word(word);
    return s;
}
