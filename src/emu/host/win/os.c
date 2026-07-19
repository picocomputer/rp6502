/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windows host-OS primitives (emu/host/host.h os_*), the Win32 counterpart of
 * posix/os.c. Several are documented no-ops because the Win32 presentation path
 * already provides the behavior (D3D11 Present paces the frame loop; MSVC's
 * struct tm has no timezone fields and strftime uses the thread locale).
 */

#include "emu/host/host.h"
#include "ria/api/oem.h"
#include "emu/host/win/win.h"
#include <direct.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

/* ---- entropy ---- */

uint64_t os_entropy_64(void)
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

uint64_t os_mono_ns(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

void os_sleep_until_ns(uint64_t target)
{
    (void)target; /* the D3D11 Present already paces the loop */
}

/* ---- broken-down time ---- */

void os_localtime(time_t t, struct tm *out)
{
    localtime_s(out, &t);
}

void os_gmtime(time_t t, struct tm *out)
{
    gmtime_s(out, &t);
}

/* ---- host-locale strftime ---- */

void os_locale_reset(void) {} /* MSVC strftime uses the thread locale directly */

size_t os_strftime_local(char *buf, size_t max, const char *fmt, const struct tm *tm)
{
    return strftime(buf, max, fmt, tm);
}

void os_tm_apply_zone(struct tm *tm, const struct tm *probe)
{
    (void)tm, (void)probe; /* MSVC struct tm carries no tm_gmtoff/tm_zone */
}

/* ---- config location ---- */

bool os_config_dir(char *buf, size_t sz)
{
    const char *base = getenv("APPDATA");
    if (!base || !base[0])
        return false;
    snprintf(buf, sz, "%s\\rp6502-emu", base);
    return true;
}

/* GUI-subsystem processes don't inherit an interactive console's stdio. */
void os_console_attach(void)
{
    HANDLE pre_out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE pre_err = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE pre_in = GetStdHandle(STD_INPUT_HANDLE);
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    if (!pre_out || pre_out == INVALID_HANDLE_VALUE)
        freopen("CONOUT$", "w", stdout);
    if (!pre_err || pre_err == INVALID_HANDLE_VALUE)
        freopen("CONOUT$", "w", stderr);
    if (!pre_in || pre_in == INVALID_HANDLE_VALUE)
        freopen("CONIN$", "r", stdin);
}

void os_ensure_parent_dir(const char *filepath)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", filepath);
    char *s1 = strrchr(tmp, '/');
    char *s2 = strrchr(tmp, '\\');
    char *slash = (s2 > s1) ? s2 : s1;
    if (!slash || slash == tmp)
        return;
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
}

/* The ANSI main()'s argv is in the process ACP, not UTF-8. */
bool os_argv_to_oem(const char *arg, char *dst, size_t dstsz)
{
    wchar_t w[4096];
    if (!MultiByteToWideChar(CP_ACP, 0, arg, -1, w, (int)(sizeof w / sizeof *w)))
        return false;
    if (wcslen(w) >= dstsz) /* one OEM byte per UTF-16 unit */
        return false;
    oem_from_wide((const uint16_t *)w, dst, dstsz);
    return true;
}

/* ---- test-only helpers ---- */

bool os_make_tmpdir(char *buf, size_t sz)
{
    wchar_t tmp[MAX_PATH], name[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp) == 0)
        return false;
    /* GetTempFileNameW makes a uniquely-named file; drop it and reuse the name
     * for a directory (the Win32 stand-in for mkdtemp). */
    if (GetTempFileNameW(tmp, L"rp6", 0, name) == 0)
        return false;
    _wunlink(name);
    if (_wmkdir(name) != 0)
        return false;
    oem_from_wide((const uint16_t *)name, buf, sz);
    win_to_slash(buf);
    return true;
}

void os_setenv(const char *name, const char *value)
{
    _putenv_s(name, value);
}
