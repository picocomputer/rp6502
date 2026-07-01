/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This platform's stdio driver table for the vendored ria/api/std.c: ROM:
 * assets first, then the RAM FatFs (only while --tmpdrive is mounted), then
 * the native host filesystem as the always-true catch-all, last. Also holds
 * the machine-reset hook and the console device names std.c matches on.
 */

#include "emu/api/std.h"
#include "emu/host/dir.h"
#include "emu/host/fs.h"
#include "emu/mon/rom.h"
#include "emu/sys/com.h"
#include "emu/usb/msc.h"
#include "api/dir.h"
#include "api/fat.h"
#include "str/rln.h"

const char STR_CON_COLON[] = "CON:";
const char STR_TTY_COLON[] = "TTY:";

/* The RAM FatFs (the shared fat_std_* driver) claims MSC0: only while
 * --tmpdrive is mounted; otherwise the host catch-all reclaims it. */
static bool emu_fat_handles(const char *path)
{
    (void)path;
    return emu_fat_active();
}

const std_driver_t std_drivers[] = {
    {rom_std_handles, rom_std_open, rom_std_close, rom_std_read, NULL, NULL, rom_std_lseek},
    {emu_fat_handles, fat_std_open, fat_std_close, fat_std_read, fat_std_write, fat_std_sync, fat_std_lseek},
    {host_std_handles, host_std_open, host_std_close, host_std_read, host_std_write, host_std_sync, host_std_lseek},
};

const size_t std_driver_count = sizeof(std_drivers) / sizeof(std_drivers[0]);

void std_reset(void)
{
    std_stop();      /* close open files, reset the in-flight op + rln read */
    std_init();      /* re-establish the console streams (fd 0-4) */
    dir_stop();      /* close open FatFs directories (ria/api/dir.c) */
    host_dir_stop(); /* close open host directories */
    com_reset();
    rln_init();
}
