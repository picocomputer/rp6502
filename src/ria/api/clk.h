/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_CLK_H_
#define _RIA_API_CLK_H_

/* The CLK driver converts time for the 6502.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

/* The 18-byte wire struct tm the 6502 libc pushes for gmtime/localtime/mktime/
 * strftime (9 int16, struct-tm order; all-int16, so it needs no packing). */
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

/* The API implementation for time support
 */

bool clk_api_time_get(void);
bool clk_api_time_set(void);
bool clk_api_gmtime(void);
bool clk_api_localtime(void);
bool clk_api_mktime(void);
bool clk_api_strftime(void);

// Deprecated. Retained for binaries built with older SDKs.
bool clk_api_clock(void);
bool clk_api_tzset(void);
bool clk_api_tzquery(void);
bool clk_api_get_res(void);
bool clk_api_get_time(void);
bool clk_api_set_time(void);

#endif /* _RIA_API_CLK_H_ */
