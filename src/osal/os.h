/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _OSAL_OS_H_
#define _OSAL_OS_H_

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* This is not reproducible, so nothing that has to repeat may call it. Each
 * host calls it once and keeps the value as its run seed. */
uint32_t os_random(void);

bool os_localtime(time_t t, struct tm *out);
bool os_gmtime(time_t t, struct tm *out);

/* strftime in the locale the environment names, loaded into a locale_t of its
 * own so the rest of the process stays in the C locale. The Windows CRT has no
 * such load and answers in the C locale. */
void os_locale_reset(void);
void os_locale_free(void);
size_t os_strftime_local(char *buf, size_t max, const char *fmt, const struct tm *tm);
void os_tm_apply_zone(struct tm *tm, const struct tm *probe);

/* One command-line argument, from this process's argv encoding to the guest's
 * OEM code page. False if it does not fit. */
bool os_argv_to_oem(const char *arg, char *dst, size_t dstsz);

/* Monotonic time, in nanoseconds from an origin only the OS knows. */
uint64_t os_mono_ns(void);

void os_sleep_ns(uint64_t ns);

/* Where an application's config file goes, in the host's native path spelling
 * and native path encoding rather than the OEM code page the drive speaks.
 * os_config_dir allocates and the caller frees; it is NULL when the host names
 * no such directory. os_ensure_parent_dir makes the directories that will hold
 * filepath. */
char *os_config_dir(void);
void os_ensure_parent_dir(const char *filepath);

#endif /* _OSAL_OS_H_ */
