/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * POSIX-family host-OS primitives common to every POSIX host (emu/plat.h os_*),
 * the counterpart of win/os.c. The two primitives that differ by OS —
 * os_entropy_64 and os_sleep_until_ns — live in the per-host os.c
 * (linux/macos/web/android); this file holds only what they all share.
 */

#include "emu/plat.h"
#include "ria/api/oem.h"
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- monotonic clock ---- */

uint64_t os_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- broken-down time ---- */

void os_localtime(time_t t, struct tm *out)
{
    localtime_r(&t, out);
}

void os_gmtime(time_t t, struct tm *out)
{
    gmtime_r(&t, out);
}

/* ---- host-locale strftime ---- */

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

/* ---- config location ---- */

bool os_config_dir(char *buf, size_t sz)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        snprintf(buf, sz, "%s/rp6502-emu", xdg);
    else
    {
        const char *home = getenv("HOME");
        if (!home || !home[0])
            return false;
        snprintf(buf, sz, "%s/.config/rp6502-emu", home);
    }
    return true;
}

void os_ensure_parent_dir(const char *filepath)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", filepath);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return;
    *slash = 0;
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/')
        {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

/* POSIX (and Emscripten) argv arrives as UTF-8. */
bool os_argv_to_oem(const char *arg, char *dst, size_t dstsz)
{
    return oem_from_utf8(arg, dst, dstsz) < dstsz;
}

/* ---- test-only helpers ---- */

bool os_make_tmpdir(char *buf, size_t sz)
{
    char tmpl[] = "/tmp/rp6502_test_XXXXXX";
    const char *d = mkdtemp(tmpl);
    if (!d || strlen(d) >= sz)
        return false;
    memcpy(buf, d, strlen(d) + 1);
    return true;
}

void os_setenv(const char *name, const char *value)
{
    setenv(name, value, 1);
}
