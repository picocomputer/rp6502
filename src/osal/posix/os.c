/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "osal/os.h"
#include "core/sys/sst.h"
#include "core/tim/tim.h"
#include <errno.h>
#if !defined(__ANDROID__)
#include <langinfo.h>
#endif
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint64_t os_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void os_sleep_ns(uint64_t ns)
{
    struct timespec ts = {
        .tv_sec = (time_t)(ns / 1000000000ull),
        .tv_nsec = (long)(ns % 1000000000ull),
    };
    nanosleep(&ts, NULL);
}

void os_zone(int64_t utc, int32_t *offset, bool *dst, char *name, size_t name_size)
{
    time_t t = (time_t)utc;
    struct tm tm;
    *offset = 0, *dst = false, name[0] = 0;
    if (!localtime_r(&t, &tm))
        return;
    *offset = (int32_t)tm.tm_gmtoff;
    *dst = tm.tm_isdst > 0;
    if (tm.tm_zone)
        snprintf(name, name_size, "%s", tm.tm_zone);
}

/* The environment's locale, used only for the words of a time so that the
 * rest of the process stays in the C locale. NULL when that locale is not
 * installed, and the C locale answers instead. */
static locale_t g_locale;

#define OSAL_OS_SST(X) X(SKIP, g_locale, "the host's")
SST_TABLE(osal_os, OSAL_OS_SST)

void os_locale_reset(void)
{
    if (!g_locale)
        g_locale = newlocale(LC_ALL_MASK, "", (locale_t)0);
}

/* A host that unloads this library takes the pointer with it, so the handle
 * is given back when the machine stops rather than at exit. */
void os_locale_free(void)
{
    if (g_locale)
        freelocale(g_locale), g_locale = (locale_t)0;
}

#if defined(__APPLE__)
char *nl_langinfo_l(nl_item, locale_t);
#endif

/* Android's C library has the C locale and no other, and asks nothing of
 * the device's language, so the words are left to the caller. */
#if defined(__ANDROID__)
const char *os_time_word(int word)
{
    (void)word;
    return NULL;
}
#else
const char *os_time_word(int word)
{
    static const nl_item items[TIM_WORD_COUNT] = {
        DAY_1, DAY_2, DAY_3, DAY_4, DAY_5, DAY_6, DAY_7,
        ABDAY_1, ABDAY_2, ABDAY_3, ABDAY_4, ABDAY_5, ABDAY_6, ABDAY_7,
        MON_1, MON_2, MON_3, MON_4, MON_5, MON_6,
        MON_7, MON_8, MON_9, MON_10, MON_11, MON_12,
        ABMON_1, ABMON_2, ABMON_3, ABMON_4, ABMON_5, ABMON_6,
        ABMON_7, ABMON_8, ABMON_9, ABMON_10, ABMON_11, ABMON_12,
        AM_STR, PM_STR, D_T_FMT, D_FMT, T_FMT, T_FMT_AMPM};
    if (word < 0 || word >= TIM_WORD_COUNT)
        return NULL;
    return g_locale ? nl_langinfo_l(items[word], g_locale) : nl_langinfo(items[word]);
}
#endif

/* The XDG Base Directory spec requires mode 0700 for every folder created
 * here, and a folder that already exists keeps its permissions. */
void os_ensure_parent_dir(const char *filepath)
{
    char *tmp = strdup(filepath); /* the separators are cut and restored in place */
    if (!tmp)
        return;
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
    {
        free(tmp);
        return;
    }
    *slash = 0;
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/')
        {
            *p = 0;
            mkdir(tmp, 0700);
            *p = '/';
        }
    mkdir(tmp, 0700);
    free(tmp);
}

FILE *os_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}
