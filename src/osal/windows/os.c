/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/os.h"
#include "core/str/oem.h"
#include <direct.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <windows.h>

/* The value is the same on every SDK; only the newer ones declare it. */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

uint32_t os_random(void)
{
    LARGE_INTEGER f, c;
    FILETIME ft;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    GetSystemTimeAsFileTime(&ft);
    uint64_t s = (uint64_t)c.QuadPart * 6364136223846793005ull +
                 ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) +
                 (uint64_t)(uintptr_t)&f + (uint64_t)f.QuadPart;
    return (uint32_t)(s ^ (s >> 32));
}

/* Integer throughout, because a double holds whole nanoseconds exactly only to
 * 2^53, which is about 104 days of uptime. The counter is split into whole
 * seconds and a remainder so the multiply cannot overflow: a plain
 * t * 1000000000 passes 2^64 after about half an hour on the 10 MHz counter
 * Windows usually reports. The frequency is fixed after boot, so it is read
 * once. */
uint64_t os_mono_ns(void)
{
    static LARGE_INTEGER f;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t hz = (uint64_t)f.QuadPart;
    const uint64_t t = (uint64_t)c.QuadPart;
    return (t / hz) * 1000000000ull + (t % hz) * 1000000000ull / hz;
}

/* A high-resolution waitable timer wakes on time, where Sleep rounds up to the
 * scheduler's tick. Windows before 1803 refuses the flag and falls back to
 * Sleep, so a wake there can be a tick late; the caller measures when it woke
 * and carries the difference forward. */
void os_sleep_ns(uint64_t ns)
{
    static HANDLE timer;
    static bool no_timer;
    if (!timer && !no_timer)
    {
        timer = CreateWaitableTimerExW(NULL, NULL,
                                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        no_timer = !timer;
    }
    if (no_timer)
    {
        Sleep((DWORD)(ns / 1000000ull));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(ns / 100ull); /* relative, 100 ns units */
    SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
    WaitForSingleObject(timer, INFINITE);
}

bool os_localtime(time_t t, struct tm *out)
{
    return localtime_s(out, &t) == 0;
}

bool os_gmtime(time_t t, struct tm *out)
{
    return gmtime_s(out, &t) == 0;
}

/* Nothing to load. No setlocale call anywhere moves the CRT out of the C
 * locale, so the strftime below answers in it. */
void os_locale_reset(void) {}
void os_locale_free(void) {}

size_t os_strftime_local(char *buf, size_t max, const char *fmt, const struct tm *tm)
{
    return strftime(buf, max, fmt, tm);
}

void os_tm_apply_zone(struct tm *tm, const struct tm *probe)
{
    (void)tm, (void)probe; /* the CRT's struct tm has no tm_gmtoff or tm_zone */
}

char *os_config_dir(void)
{
    const char *base = getenv("APPDATA");
    if (!base || !base[0])
        return NULL;
    static const char tail[] = "\\rp6502-emu";
    char *dir = malloc(strlen(base) + sizeof tail);
    if (dir)
        sprintf(dir, "%s%s", base, tail);
    return dir;
}

void os_ensure_parent_dir(const char *filepath)
{
    char *tmp = strdup(filepath);
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

/* An ANSI main()'s argv is in the process ANSI code page, not UTF-8. */
bool os_argv_to_oem(const char *arg, char *dst, size_t dstsz)
{
    int n = MultiByteToWideChar(CP_ACP, 0, arg, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (!w || !MultiByteToWideChar(CP_ACP, 0, arg, -1, w, n))
    {
        free(w);
        return false;
    }
    bool ok = wcslen(w) < dstsz; /* one OEM byte per UTF-16 unit */
    if (ok)
        oem_from_wide((const uint16_t *)w, dst, dstsz);
    free(w);
    return ok;
}
