/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What this machine has for a program to open and for a path to reach: the
 * stdio driver table, and the one drive behind the directory calls. Both are
 * a machine's own -- the RIA offers six drivers and a FatFs volume, this
 * offers two and the host's filesystem.
 *
 * The Pocket's table (host/pocket/sw/main.c) is the same two rows written
 * again. Unifying them changes what that machine links, so it is not done
 * here, but it is worth knowing before a third copy appears.
 */

#include "core/api/dir.h"
#include "core/api/std.h"
#include "core/sys/msc.h"
#include "core/sys/rom.h"

const dir_backend_t *dir_backend(void)
{
    return &msc_dir_backend;
}

static const std_driver_t std_driver_table[] = {
    {rom_std_handles, rom_std_open, rom_std_close, rom_std_read, NULL, NULL, rom_std_lseek},
    {msc_std_handles, msc_std_open, msc_std_close, msc_std_read, msc_std_write, msc_std_sync, msc_std_lseek},
};

const std_driver_t *std_drivers(size_t *count)
{
    *count = sizeof(std_driver_table) / sizeof(std_driver_table[0]);
    return std_driver_table;
}
