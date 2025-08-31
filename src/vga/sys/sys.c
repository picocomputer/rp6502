/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <pico.h>

__in_flash("vga_sys_sys") static const char SYS_VERSION[] =
    "VGA "
#if RP6502_VERSION_EMPTY
    __DATE__ " " __TIME__
#else
    "Version " RP6502_VERSION
#endif
    ;

__in_flash("sys_version") const char *sys_version(void)
{
    return SYS_VERSION;
}
