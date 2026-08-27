/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What this machine offers a program to open, and the one drive a path
 * reaches. Both are the machine's: which drivers exist is the same kind of
 * fact as which drivers come up, and belongs beside the roster that says so.
 */

#include "host.h"

#include "core/api/dir.h"
#include "core/api/std.h"
#include "ria/api/fat.h"
#include "ria/mon/rom.h"
#include "ria/net/modem.h"
#include "ria/usb/mid.h"
#include "ria/usb/nfc.h"
#include "ria/usb/vcp.h"

// Driver table, msc is catch-all and must be last.
const dir_backend_t *dir_backend(void)
{
    return &fat_dir_backend;
}

static HOST_IN_FLASH("std_drivers") const std_driver_t std_driver_table[] = {
    {modem_std_handles, modem_std_open, modem_std_close, modem_std_read, modem_std_write, NULL, NULL},
    {vcp_std_handles, vcp_std_open, vcp_std_close, vcp_std_read, vcp_std_write, NULL, NULL},
    {mid_std_handles, mid_std_open, mid_std_close, mid_std_read, mid_std_write, mid_std_sync, NULL},
    {rom_std_handles, rom_std_open, rom_std_close, rom_std_read, NULL, NULL, rom_std_lseek},
    {nfc_std_handles, nfc_std_open, nfc_std_close, nfc_std_read, nfc_std_write, NULL, NULL},
    {fat_std_handles, fat_std_open, fat_std_close, fat_std_read, fat_std_write, fat_std_sync, fat_std_lseek},
};

const std_driver_t *std_drivers(size_t *count)
{
    *count = sizeof(std_driver_table) / sizeof(std_driver_table[0]);
    return std_driver_table;
}
