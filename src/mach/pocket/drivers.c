/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What this machine offers a program to open, and the one drive a path
 * reaches. Both are the machine's: which drivers exist is the same kind of
 * fact as which drivers come up, and belongs beside the roster that says so.
 */

#include "core/api/dir.h"
#include "core/api/std.h"
#include "sw/msc.h"
#include "sw/rom.h"

const dir_backend_t *dir_backend(void)
{
    return &msc_dir_backend;
}

static const std_driver_t std_driver_table[] = {
    {
        .handles = rom_std_handles,
        .open = rom_std_open,
        .close = rom_std_close,
        .read = rom_std_read,
        .lseek = rom_std_lseek,
    },
    {
        .handles = msc_std_handles,
        .open = msc_std_open,
        .close = msc_std_close,
        .read = msc_std_read,
        .write = msc_std_write,
        .sync = msc_std_sync,
        .lseek = msc_std_lseek,
    },
};

const std_driver_t *std_drivers(size_t *count)
{
    *count = sizeof std_driver_table / sizeof std_driver_table[0];
    return std_driver_table;
}
