/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hostcompat.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#include "emu/api/oem.h"

bool host_make_tmpdir(char *out, size_t outsz)
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
    oem_from_wide((const uint16_t *)name, out, outsz);
    for (char *p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
    return true;
}

void host_setenv(const char *name, const char *value)
{
    _putenv_s(name, value);
}

#else

bool host_make_tmpdir(char *out, size_t outsz)
{
    char tmpl[] = "/tmp/rp6502_test_XXXXXX";
    const char *d = mkdtemp(tmpl);
    if (!d || strlen(d) >= outsz)
        return false;
    memcpy(out, d, strlen(d) + 1);
    return true;
}

void host_setenv(const char *name, const char *value)
{
    setenv(name, value, 1);
}

#endif
