/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_CLK_H_
#define _CORE_API_CLK_H_

#include <stddef.h>
#include <stdint.h>
#include "core/sys/sst.h"
#include <stdbool.h>
#include <string.h>
#include <time.h>

void clk_run(void);

uint32_t clk_get_run(uint32_t us_per_tick);

/* The struct tm exchanged with the 6502 libc: gmtime and localtime push it,
 * mktime and strftime receive it. Every field is an int16 in struct tm order,
 * so it needs no packing. */
struct clk_wire_tm
{
    int16_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon;
    int16_t tm_year, tm_wday, tm_yday, tm_isdst;
};
_Static_assert(18 == sizeof(struct clk_wire_tm), "wire struct tm");

static inline void clk_tm_to_wire(const struct tm *tm, struct clk_wire_tm *w)
{
    w->tm_sec = tm->tm_sec, w->tm_min = tm->tm_min, w->tm_hour = tm->tm_hour;
    w->tm_mday = tm->tm_mday, w->tm_mon = tm->tm_mon, w->tm_year = tm->tm_year;
    w->tm_wday = tm->tm_wday, w->tm_yday = tm->tm_yday, w->tm_isdst = tm->tm_isdst;
}

static inline void clk_wire_to_tm(const struct clk_wire_tm *w, struct tm *tm)
{
    memset(tm, 0, sizeof(*tm));
    tm->tm_sec = w->tm_sec, tm->tm_min = w->tm_min, tm->tm_hour = w->tm_hour;
    tm->tm_mday = w->tm_mday, tm->tm_mon = w->tm_mon, tm->tm_year = w->tm_year;
    tm->tm_wday = w->tm_wday, tm->tm_yday = w->tm_yday, tm->tm_isdst = w->tm_isdst;
}

bool clk_api_time_get(void);
bool clk_api_time_set(void);
bool clk_api_gmtime(void);
bool clk_api_localtime(void);
bool clk_api_mktime(void);
bool clk_api_strftime(void);

// Deprecated. Retained for binaries built with older SDKs.
bool clk_api_clock(void);
bool clk_api_get_res(void);
bool clk_api_get_time(void);
bool clk_api_set_time(void);

/* The host clock reading taken when the running program started, which is
 * what clk_get_run measures against. Every machine that saves state also
 * restores that clock, so the reading means the same thing after a load as it
 * did when it was written. */
#define CLK_SST_SIZE 8
void clk_sst_save(sst_cursor_t *c, unsigned flags);
bool clk_sst_load(sst_cursor_t *c, unsigned flags);

#define CLK_DRIVER DRIVER(nul_init, nul_task, nul_task, clk_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(CLK_, 1, CLK_SST_SIZE, clk_sst_save, clk_sst_load))

#endif /* _CORE_API_CLK_H_ */
