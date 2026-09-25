/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/os.h"
#include "osal/windows/dir.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <windows.h>
/* initguid.h comes before knownfolders.h so that this file defines
 * FOLDERID_SavedGames itself, and no uuid library is needed for it. */
#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>

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

/* %a, %b and %c come out in English here, while the POSIX hosts format in the
 * environment's locale. This needs upgrading, if possible, to _strftime_l with
 * a locale from _create_locale(LC_TIME, ""). */
size_t os_strftime_local(char *buf, size_t max, const char *fmt, const struct tm *tm)
{
    return strftime(buf, max, fmt, tm);
}

void os_tm_apply_zone(struct tm *tm, const struct tm *probe)
{
    (void)tm, (void)probe; /* the CRT's struct tm has no tm_gmtoff or tm_zone */
}

/* A folder path from the wide API joined with an ASCII tail, as one UTF-8
 * path. */
static char *win_join_utf8(const wchar_t *base, const char *tail)
{
    char *u8 = win_wide_to_utf8(base);
    char *dir = u8 ? realloc(u8, strlen(u8) + strlen(tail) + 1) : NULL;
    if (!dir)
    {
        free(u8);
        return NULL;
    }
    strcat(dir, tail);
    return dir;
}

char *os_config_dir(void)
{
    const wchar_t *base = _wgetenv(L"APPDATA");
    return base && base[0] ? win_join_utf8(base, "\\rp6502-emu") : NULL;
}

void os_ensure_parent_dir(const char *filepath)
{
    api_errno ignored;
    wchar_t *w = win_utf8_to_wide(filepath, &ignored);
    if (w)
        win_make_parents(w);
    free(w);
}

/* KF_FLAG_CREATE makes Saved Games when a profile lacks it. The folder inside
 * it is made by the first SAVE: open that creates a file. */
char *os_save_dir(void)
{
    PWSTR base = NULL;
    char *dir = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_SavedGames, KF_FLAG_CREATE, NULL, &base)))
        dir = win_join_utf8(base, "\\rp6502");
    CoTaskMemFree(base);
    return dir;
}

FILE *os_fopen(const char *path, const char *mode)
{
    api_errno ignored;
    wchar_t *wpath = win_utf8_to_wide(path, &ignored);
    wchar_t *wmode = win_utf8_to_wide(mode, &ignored);
    FILE *f = wpath && wmode ? _wfopen(wpath, wmode) : NULL;
    free(wpath);
    free(wmode);
    return f;
}
