/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tb_hostos.h"

#include "core/str/oem.h"
#include "osal/windows/errmap.h"
#include <direct.h>
#include <errno.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

bool host_make_tmpdir(char *buf, size_t sz)
{
    wchar_t tmp[MAX_PATH], name[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp) == 0)
        return false;
    for (unsigned i = 0; i < 4096; i++)
    {
        if (swprintf(name, MAX_PATH, L"%srp6502_test_%lu_%u", tmp,
                     (unsigned long)GetCurrentProcessId(), i) < 0)
            return false;
        if (_wmkdir(name) == 0)
        {
            oem_from_wide((const uint16_t *)name, buf, sz);
            win_to_slash(buf);
            return true;
        }
        if (errno != EEXIST)
            return false;
    }
    return false;
}

void host_setenv(const char *name, const char *value)
{
    _putenv_s(name, value);
}

const char *host_drive(void)
{
    static char drive[3];
    wchar_t cwd[MAX_PATH];
    DWORD n = GetCurrentDirectoryW(MAX_PATH, cwd);
    if (n && n < MAX_PATH && cwd[0] && cwd[1] == L':')
    {
        drive[0] = (char)cwd[0];
        drive[1] = ':';
        drive[2] = 0;
    }
    return drive;
}
