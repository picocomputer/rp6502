/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The two helpers tb_hostos.h declares, per host OS. They were bodies in
 * host/<os>/host.c, which put a temp-directory maker and a setenv into
 * emu_core -- and so into the emulator, the core, the bundle and the APK,
 * none of which has ever called them.
 */

#include "tb_hostos.h"

#ifdef _WIN32

#include "core/api/oem.h"
#include "host/windows/win.h"
#include <direct.h>
#include <io.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

bool host_make_tmpdir(char *buf, size_t sz)
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

void host_setenv(const char *name, const char *value)
{
    _putenv_s(name, value);
}

#else

#include <stdlib.h>
#include <string.h>

bool host_make_tmpdir(char *buf, size_t sz)
{
    char tmpl[] = "/tmp/rp6502_test_XXXXXX";
    const char *d = mkdtemp(tmpl);
    if (!d || strlen(d) >= sz)
        return false;
    memcpy(buf, d, strlen(d) + 1);
    return true;
}

void host_setenv(const char *name, const char *value)
{
    setenv(name, value, 1);
}

#endif
