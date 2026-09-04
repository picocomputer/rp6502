/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The two helpers tb_hostos.h declares, on Windows. The build picks this
 * file or its POSIX sibling; neither carries the other's spelling.
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
    /* mkdtemp's contract is that the directory is what gets made. The Win32
     * idiom for it -- GetTempFileNameW, unlink, mkdir -- reuses a name the
     * filesystem may still be holding: a delete does not take effect until the
     * last handle closes, so a scanner that opened the file to look at it makes
     * the mkdir fail. It is rare, it is not reproducible, and it fails the whole
     * suite when it lands. Take a name nobody has created instead, and let the
     * mkdir itself be the thing that claims it. */
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
