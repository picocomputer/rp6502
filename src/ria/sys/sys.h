/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_SYS_H_
#define _RIA_SYS_SYS_H_

#define SYS_TZ_MAX_SIZE 64

/* System monitor commands, the real time clock, and the run timer.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* Main events
 */

void sys_init(void);
void sys_run(void);

/* Monitor commands
 */

void sys_mon_reboot(const char *args);
void sys_mon_reset(const char *args);
void sys_mon_status(const char *args);

// Print for status command
int sys_status_response(char *buf, size_t buf_size, int state, unsigned width);

// Show tz database
int sys_tzdata_response(char *buf, size_t buf_size, int state, unsigned width);

// Configuration setting TZ
// Use POSIX TZ format. e.g. PST8PDT,M3.2.0/2,M11.1.0/2
void sys_load_time_zone(const char *str);
bool sys_set_time_zone(const char *tz);
const char *sys_get_time_zone(void);

// POSIX tzset() globals, which every libc spells differently
int sys_get_tz_daylight(void);
long sys_get_tz_offset(void);

// Real time clock
bool sys_get_time(struct timespec *ts);
bool sys_set_time(const struct timespec *ts);
void sys_get_time_res(struct timespec *ts);

// 6502 run time in ticks of us_per_tick microseconds
uint32_t sys_get_run(uint32_t us_per_tick);

// strftime emitting code page text
size_t sys_strftime(char *dst, size_t max, const char *format, const struct tm *tm);

#endif /* _RIA_SYS_SYS_H_ */
