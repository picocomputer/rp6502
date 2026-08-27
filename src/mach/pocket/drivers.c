/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What this machine offers a program to open. Which drivers exist is the same
 * kind of fact as which drivers come up, and belongs beside the roster that
 * says so. The drive a path reaches is not listed here: core/api/dir.h names
 * those calls and the host that is linked defines them.
 */

#include "core/api/dir.h"
#include "core/api/std.h"
#include "sw/fs.h"
#include "sw/rom.h"

static const std_driver_t std_driver_table[] = {
    {
        .handles = rom_std_handles,
        .open = rom_std_open,
        .close = rom_std_close,
        .read = rom_std_read,
        .lseek = rom_std_lseek,
    },
    {
        .handles = fs_std_handles,
        .open = fs_std_open,
        .close = fs_std_close,
        .read = fs_std_read,
        .write = fs_std_write,
        .sync = fs_std_sync,
        .lseek = fs_std_lseek,
    },
};

const std_driver_t *std_drivers(size_t *count)
{
    *count = sizeof std_driver_table / sizeof std_driver_table[0];
    return std_driver_table;
}
