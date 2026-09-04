/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/os.h"
#include "core/str/oem.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

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

/* The ANSI main()'s argv is in the process ACP, not UTF-8. */
bool os_argv_to_oem(const char *arg, char *dst, size_t dstsz)
{
    int n = MultiByteToWideChar(CP_ACP, 0, arg, -1, NULL, 0); /* asks its own size */
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
