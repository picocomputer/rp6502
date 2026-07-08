/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/api/std.h"
#include "emu/host/dir.h"
#include "emu/host/msc.h"
#include "emu/mon/rom.h"
#include "emu/sys/com.h"
#include "emu/host/fat.h"
#include "api/dir.h"
#include "api/fat.h"
#include "str/rln.h"

const char STR_CON_COLON[] = "CON:";
const char STR_TTY_COLON[] = "TTY:";

/* The RAM FatFs (the shared fat_std_* driver) claims MSC0: only while
 * --tmpdrive is mounted; otherwise the host catch-all reclaims it. */
static bool fat_handles(const char *path)
{
    (void)path;
    return hostfat_active();
}

const std_driver_t std_drivers[] = {
    {rom_std_handles, rom_std_open, rom_std_close, rom_std_read, NULL, NULL, rom_std_lseek},
    {fat_handles, fat_std_open, fat_std_close, fat_std_read, fat_std_write, fat_std_sync, fat_std_lseek},
    {msc_std_handles, msc_std_open, msc_std_close, msc_std_read, msc_std_write, msc_std_sync, msc_std_lseek},
};

const size_t std_driver_count = sizeof(std_drivers) / sizeof(std_drivers[0]);

void std_reset(void)
{
    std_stop();      /* close open files, reset the in-flight op + rln read */
    std_init();      /* re-establish the console streams (fd 0-4) */
    dir_stop();      /* close open FatFs directories (ria/api/dir.c) */
    hostdir_stop(); /* close open host directories */
    com_set_bel(true); /* reset BEL per program start; type-ahead in the com rings
                          survives across exec (firmware parity — com_reset, the
                          full flush, is cold-boot only) */
    rln_init();
}
