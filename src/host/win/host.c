/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Win32 primitives that are this program's rather than the operating
 * system's. What Windows answers for every host of ours is in
 * host/windows/host.c; these three differ because the emulator is a program
 * with a window and a console and an ANSI main(), and a libretro core is
 * none of those things.
 */

#include "host.h"
#include "core/api/oem.h"
#include "host/sokol/cli.h"    /* host_console_attach */
#include "host/sokol/window.h" /* host_sleep_until_ns */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

void host_sleep_until_ns(uint64_t target)
{
    (void)target; /* the D3D11 Present already paces the loop */
}

/* ---- broken-down time ---- */

void host_console_attach(void)
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
bool host_argv_to_oem(const char *arg, char *dst, size_t dstsz)
{
    wchar_t w[4096];
    if (!MultiByteToWideChar(CP_ACP, 0, arg, -1, w, (int)(sizeof w / sizeof *w)))
        return false;
    if (wcslen(w) >= dstsz) /* one OEM byte per UTF-16 unit */
        return false;
    oem_from_wide((const uint16_t *)w, dst, dstsz);
    return true;
}
