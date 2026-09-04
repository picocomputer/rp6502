/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/os.h"
#include "core/str/oem.h"
#include <errno.h>
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

bool os_localtime(time_t t, struct tm *out)
{
    return localtime_r(&t, out) != NULL;
}

bool os_gmtime(time_t t, struct tm *out)
{
    return gmtime_r(&t, out) != NULL;
}

#if defined(__APPLE__)
size_t strftime_l(char *restrict, size_t, const char *restrict,
                  const struct tm *restrict, locale_t);
#endif

/* Host locale used only for strftime, so the rest of the process stays in the
 * C locale. NULL if the environment locale isn't installed (falls back to C). */
static locale_t g_locale;

void os_locale_reset(void)
{
    if (!g_locale)
        g_locale = newlocale(LC_ALL_MASK, "", (locale_t)0);
}

size_t os_strftime_local(char *buf, size_t max, const char *fmt, const struct tm *tm)
{
    return g_locale ? strftime_l(buf, max, fmt, tm, g_locale)
                    : strftime(buf, max, fmt, tm);
}

void os_tm_apply_zone(struct tm *tm, const struct tm *probe)
{
#if defined(__GLIBC__) || defined(__APPLE__) || defined(__EMSCRIPTEN__) || defined(__USE_MISC)
    tm->tm_gmtoff = probe->tm_gmtoff;
    tm->tm_zone = probe->tm_zone;
#else
    (void)tm, (void)probe;
#endif
}

char *os_config_dir(void)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    const char *tail = "/rp6502-emu";
    if (!base || !base[0])
    {
        base = getenv("HOME");
        tail = "/.config/rp6502-emu";
    }
    if (!base || !base[0])
        return NULL;
    /* An environment variable is as long as the environment made it. */
    char *dir = malloc(strlen(base) + strlen(tail) + 1);
    if (dir)
        sprintf(dir, "%s%s", base, tail);
    return dir;
}

void os_ensure_parent_dir(const char *filepath)
{
    char *tmp = strdup(filepath); /* walked in place, so it is ours */
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
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
    free(tmp);
}

void os_console_attach(void) {}

/* POSIX (and Emscripten) argv arrives as UTF-8. */
bool os_argv_to_oem(const char *arg, char *dst, size_t dstsz)
{
    return oem_from_utf8(arg, dst, dstsz) < dstsz;
}
