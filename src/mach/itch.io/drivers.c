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
#include "core/api/fs.h"
#include "core/rom/rom.h"

const dir_backend_t *dir_backend(void)
{
    return &fs_dir_backend;
}

static const std_driver_t std_driver_table[] = {
    {rom_std_handles, rom_std_open, rom_std_close, rom_std_read, NULL, NULL, rom_std_lseek},
    {fs_std_handles, fs_std_open, fs_std_close, fs_std_read, fs_std_write, fs_std_sync, fs_std_lseek},
};

const std_driver_t *std_drivers(size_t *count)
{
    *count = sizeof(std_driver_table) / sizeof(std_driver_table[0]);
    return std_driver_table;
}
