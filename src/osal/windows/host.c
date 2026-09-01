/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What Windows answers for any host of ours (osal/os.h host_*), the Win32
 * counterpart of osal/posix/host.c. The emulator and the libretro core share
 * every line of it.
 *
 * What is NOT here is what differs between those two rather than between
 * operating systems: the frame-pacer sleep, which only a host that owns its
 * presentation has; the console attach, which only a program with a console
 * wants; and the argv encoding, because an ANSI main() is handed the process
 * code page while a libretro frontend hands UTF-8. Each host answers those
 * itself.
 *
 * Several are documented no-ops because MSVC's struct tm has no timezone
 * fields and its strftime uses the thread locale directly.
 */

#include "osal/os.h"
#include <direct.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

/* ---- entropy ---- */

uint64_t host_entropy_64(void)
{
    LARGE_INTEGER f, c;
    FILETIME ft;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    GetSystemTimeAsFileTime(&ft);
    uint64_t s = (uint64_t)c.QuadPart * 6364136223846793005ull +
                 ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) +
                 (uint64_t)(uintptr_t)&f + (uint64_t)f.QuadPart;
    return s ? s : 1;
}

/* ---- monotonic clock + frame-pacer sleep ---- */

uint64_t host_mono_ns(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

bool host_localtime(time_t t, struct tm *out)
{
    return localtime_s(out, &t) == 0;
}

bool host_gmtime(time_t t, struct tm *out)
{
    return gmtime_s(out, &t) == 0;
}

/* ---- host-locale strftime ---- */

void host_locale_reset(void) {} /* MSVC strftime uses the thread locale directly */

size_t host_strftime_local(char *buf, size_t max, const char *fmt, const struct tm *tm)
{
    return strftime(buf, max, fmt, tm);
}

void host_tm_apply_zone(struct tm *tm, const struct tm *probe)
{
    (void)tm, (void)probe; /* MSVC struct tm carries no tm_gmtoff/tm_zone */
}

/* ---- config location ---- */

char *host_config_dir(void)
{
    const char *base = getenv("APPDATA");
    if (!base || !base[0])
        return NULL;
    /* An environment variable is as long as the environment made it. */
    static const char tail[] = "\\rp6502-emu";
    char *dir = malloc(strlen(base) + sizeof tail);
    if (dir)
        sprintf(dir, "%s%s", base, tail);
    return dir;
}

/* GUI-subsystem processes don't inherit an interactive console's stdio. */

void host_ensure_parent_dir(const char *filepath)
{
    char *tmp = strdup(filepath); /* walked in place, so it is ours */
    if (!tmp)
        return;
    char *s1 = strrchr(tmp, '/');
    char *s2 = strrchr(tmp, '\\');
    char *slash = (s2 > s1) ? s2 : s1;
    if (!slash || slash == tmp)
    {
        free(tmp);
        return;
    }
    *slash = 0;
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/' || *p == '\\')
        {
            char c = *p;
            *p = 0;
            _mkdir(tmp);
            *p = c;
        }
    _mkdir(tmp);
    free(tmp);
}
