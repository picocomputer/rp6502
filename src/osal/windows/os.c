/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/os.h"
#include "core/sys/sst.h"
#include "osal/windows/dir.h"
#include "core/tim/tim.h"
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
static LARGE_INTEGER win_qpc_hz;

uint64_t os_mono_ns(void)
{
    if (!win_qpc_hz.QuadPart)
        QueryPerformanceFrequency(&win_qpc_hz);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t hz = (uint64_t)win_qpc_hz.QuadPart;
    const uint64_t t = (uint64_t)c.QuadPart;
    return (t / hz) * 1000000000ull + (t % hz) * 1000000000ull / hz;
}

/* A high-resolution waitable timer wakes on time, where Sleep rounds up to the
 * scheduler's tick. Windows before 1803 refuses the flag and falls back to
 * Sleep, so a wake there can be a tick late; the caller measures when it woke
 * and carries the difference forward. */
static HANDLE win_timer;
static bool win_no_timer;

void os_sleep_ns(uint64_t ns)
{
    if (!win_timer && !win_no_timer)
    {
        win_timer = CreateWaitableTimerExW(NULL, NULL,
                                           CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
        win_no_timer = !win_timer;
    }
    if (win_no_timer)
    {
        Sleep((DWORD)(ns / 1000000ull));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(ns / 100ull); /* relative, 100 ns units */
    SetWaitableTimer(win_timer, &due, 0, NULL, NULL, FALSE);
    WaitForSingleObject(win_timer, INFINITE);
}

/* The C runtime's zone: the TZ environment variable where it is set, as on
 * the other hosts, and Windows's own zone where it is not. Its names are
 * then whole ones such as "Pacific Standard Time", because Windows has no
 * short names. The runtime has no time before 1970, and the zone's standard
 * time answers for one. */
void os_zone(int64_t utc, int32_t *offset, bool *dst, char *name, size_t name_size)
{
    __time64_t t = (__time64_t)utc;
    struct tm local;
    long west = 0;
    size_t length = 0;
    *offset = 0, *dst = false, name[0] = 0;
    _tzset();
    if (utc >= 0 && _localtime64_s(&local, &t) == 0)
    {
        /* The local date read as UTC is that many seconds ahead of the time. */
        *dst = local.tm_isdst > 0;
        *offset = (int32_t)(_mkgmtime64(&local) - t);
    }
    else if (_get_timezone(&west) == 0)
        *offset = (int32_t)-west;
    if (_get_tzname(&length, name, name_size, *dst ? 1 : 0) != 0)
        name[0] = 0;
}

/* A date or time picture of Windows, such as "M/d/yyyy" or "h:mm:ss tt", as
 * a format of strftime: a run of one letter is a field, and how long the
 * run is says how the field is written. Text between single quotes is
 * itself. */
static void win_picture(const char *picture, char *out, size_t out_size)
{
    size_t n = 0;
#define PUT(text)                                         \
    for (const char *t_ = (text); *t_ && n + 1 < out_size; t_++) \
        out[n++] = *t_
    for (const char *p = picture; *p;)
    {
        if (*p == '\'')
        {
            for (p++; *p && (*p != '\'' || *++p == '\''); p++)
                if (n + 1 < out_size)
                    out[n++] = *p;
            continue;
        }
        size_t run = 1;
        while (p[run] == *p)
            run++;
        const char *field = NULL;
        switch (*p)
        {
        case 'd': field = run == 1 ? "%-d" : run == 2 ? "%d" : run == 3 ? "%a" : "%A"; break;
        case 'M': field = run == 1 ? "%-m" : run == 2 ? "%m" : run == 3 ? "%b" : "%B"; break;
        case 'y': field = run == 1 ? "%-y" : run == 2 ? "%y" : "%Y"; break;
        case 'h': field = run == 1 ? "%-I" : "%I"; break;
        case 'H': field = run == 1 ? "%-H" : "%H"; break;
        case 'm': field = run == 1 ? "%-M" : "%M"; break;
        case 's': field = run == 1 ? "%-S" : "%S"; break;
        case 't': field = "%p"; break;
        case 'g': field = ""; break; /* the era, which strftime has none of */
        case '%': field = "%%"; run = 1; break;
        }
        if (field)
        {
            PUT(field);
            p += run;
        }
        else if (n + 1 < out_size)
            out[n++] = *p++;
        else
            p++;
    }
#undef PUT
    out[n] = 0;
}

/* The words of the user's regional format, each asked for once. */
#define WIN_WORD_SIZE 96
static char win_words[TIM_WORD_COUNT][WIN_WORD_SIZE];
static bool win_word_known[TIM_WORD_COUNT];

/* The host's clock, timer and regional format, not the machine's. */
#define OSAL_OS_SST(X)                                                    \
    X(SKIP, win_qpc_hz)                                                   \
    X(SKIP, win_timer)                                                    \
    X(SKIP, win_no_timer)                                                 \
    X(SKIP, win_words)                                                    \
    X(SKIP, win_word_known)
SST_TABLE(osal_os, OSAL_OS_SST)

void os_locale_reset(void)
{
    memset(win_word_known, 0, sizeof win_word_known);
}

void os_locale_free(void)
{
}

static bool win_locale_info(LCTYPE type, char *out, size_t out_size)
{
    wchar_t wide[WIN_WORD_SIZE];
    return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type, wide, WIN_WORD_SIZE) &&
           WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_size, NULL, NULL);
}

const char *os_time_word(int word)
{
    /* Windows counts its days from Monday. */
    static const LCTYPE names[TIM_WORD_AM] = {
        LOCALE_SDAYNAME7, LOCALE_SDAYNAME1, LOCALE_SDAYNAME2, LOCALE_SDAYNAME3,
        LOCALE_SDAYNAME4, LOCALE_SDAYNAME5, LOCALE_SDAYNAME6,
        LOCALE_SABBREVDAYNAME7, LOCALE_SABBREVDAYNAME1, LOCALE_SABBREVDAYNAME2,
        LOCALE_SABBREVDAYNAME3, LOCALE_SABBREVDAYNAME4, LOCALE_SABBREVDAYNAME5,
        LOCALE_SABBREVDAYNAME6,
        LOCALE_SMONTHNAME1, LOCALE_SMONTHNAME2, LOCALE_SMONTHNAME3, LOCALE_SMONTHNAME4,
        LOCALE_SMONTHNAME5, LOCALE_SMONTHNAME6, LOCALE_SMONTHNAME7, LOCALE_SMONTHNAME8,
        LOCALE_SMONTHNAME9, LOCALE_SMONTHNAME10, LOCALE_SMONTHNAME11, LOCALE_SMONTHNAME12,
        LOCALE_SABBREVMONTHNAME1, LOCALE_SABBREVMONTHNAME2, LOCALE_SABBREVMONTHNAME3,
        LOCALE_SABBREVMONTHNAME4, LOCALE_SABBREVMONTHNAME5, LOCALE_SABBREVMONTHNAME6,
        LOCALE_SABBREVMONTHNAME7, LOCALE_SABBREVMONTHNAME8, LOCALE_SABBREVMONTHNAME9,
        LOCALE_SABBREVMONTHNAME10, LOCALE_SABBREVMONTHNAME11, LOCALE_SABBREVMONTHNAME12};
    if (word < 0 || word >= TIM_WORD_COUNT)
        return NULL;
    char *out = win_words[word];
    if (win_word_known[word])
        return out[0] || word < TIM_WORD_D_T_FMT ? out : NULL;
    win_word_known[word] = true;
    out[0] = 0;
    char date[WIN_WORD_SIZE], clock[WIN_WORD_SIZE];
    bool ok = false;
    if (word < TIM_WORD_AM)
        ok = win_locale_info(names[word], out, WIN_WORD_SIZE);
    else if (word == TIM_WORD_AM || word == TIM_WORD_PM)
        ok = win_locale_info(word == TIM_WORD_AM ? LOCALE_S1159 : LOCALE_S2359, out, WIN_WORD_SIZE);
    else if (word == TIM_WORD_D_FMT || word == TIM_WORD_D_T_FMT)
    {
        /* %c is the short date and the time, as the C runtime has it. */
        ok = win_locale_info(LOCALE_SSHORTDATE, date, sizeof date);
        if (ok && word == TIM_WORD_D_T_FMT)
        {
            ok = win_locale_info(LOCALE_STIMEFORMAT, clock, sizeof clock);
            size_t n = strlen(date);
            snprintf(date + n, sizeof date - n, " %s", clock);
        }
        if (ok)
            win_picture(date, out, WIN_WORD_SIZE);
    }
    else if (word == TIM_WORD_T_FMT)
    {
        ok = win_locale_info(LOCALE_STIMEFORMAT, clock, sizeof clock);
        if (ok)
            win_picture(clock, out, WIN_WORD_SIZE);
    }
    /* Windows has no format of its own for %r. */
    if (!ok)
    {
        out[0] = 0;
        win_word_known[word] = word >= TIM_WORD_D_T_FMT;
        return NULL;
    }
    return out;
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
