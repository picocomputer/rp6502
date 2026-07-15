/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * POSIX-family host-OS primitives (emu/plat.h os_*), the counterpart of win/os.c.
 * This is the one file that carries the intra-POSIX-family branches (Linux vs
 * macOS vs Emscripten); the callers above the seam stay platform-free.
 */

#include "emu/plat.h"
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <sys/random.h>
#endif

/* ---- entropy ---- */

uint64_t os_entropy_64(void)
{
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    {
        uint64_t s;
        if (getrandom(&s, sizeof s, 0) == (ssize_t)sizeof s && s)
            return s;
    }
#endif
    struct timespec mono = {0}, real = {0};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    uint64_t s = (uint64_t)mono.tv_nsec * 6364136223846793005ull +
                 (uint64_t)real.tv_sec * 1442695040888963407ull +
                 (uint64_t)real.tv_nsec + (uint64_t)(uintptr_t)&mono;
    return s ? s : 1;
}

/* ---- monotonic clock + frame-pacer sleep ---- */

uint64_t os_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void os_sleep_until_ns(uint64_t target)
{
#if defined(__EMSCRIPTEN__)
    (void)target; /* requestAnimationFrame paces the web loop */
#elif defined(__APPLE__)
    uint64_t now = os_mono_ns();
    if (target > now)
    {
        uint64_t delta = target - now;
        struct timespec req = {.tv_sec = (time_t)(delta / 1000000000ull),
                               .tv_nsec = (long)(delta % 1000000000ull)};
        while (nanosleep(&req, &req) != 0 && errno == EINTR)
            ;
    }
#else
    struct timespec until = {.tv_sec = (time_t)(target / 1000000000ull),
                             .tv_nsec = (long)(target % 1000000000ull)};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL);
#endif
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
