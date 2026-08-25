/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The one Win32 primitive this host answers itself. Everything else Windows
 * has to say is in host/windows/host.c, shared with the desktop emulator.
 *
 * A libretro frontend hands its paths over as UTF-8, on Windows as anywhere
 * else. The emulator's conversion is from the process code page, which is
 * right for the argv an ANSI main() is given and would mangle every
 * non-ASCII path here.
 */

#include "host.h"
#include "core/api/oem.h"

bool host_argv_to_oem(const char *arg, char *dst, size_t dstsz)
{
    return oem_from_utf8(arg, dst, dstsz) < dstsz;
}
