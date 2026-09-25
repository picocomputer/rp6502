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

/* Monotonic time, in nanoseconds from an origin only the OS knows. */
uint64_t os_mono_ns(void);

void os_sleep_ns(uint64_t ns);

/* Where an application's config file goes, as a host path in UTF-8 rather
 * than in the OEM code page the drive uses. os_config_dir allocates and the
 * caller frees; it is NULL when the host has no such directory.
 * os_ensure_parent_dir makes the directories that will hold filepath. */
char *os_config_dir(void);
void os_ensure_parent_dir(const char *filepath);

/* The folder that the host's guidelines give for an application's saved data,
 * as a host path in UTF-8, allocated for the caller to free, or NULL when the
 * guidelines give none. This only builds the path; the first SAVE: open that
 * creates a file creates any missing part of the folder. */
char *os_save_dir(void);

/* fopen of a host path in UTF-8. The Windows CRT's fopen reads the ANSI code
 * page, so Windows converts the path and calls _wfopen. */
FILE *os_fopen(const char *path, const char *mode);

#endif /* _OSAL_OS_H_ */
