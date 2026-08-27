/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What this machine offers a program to open. Which drivers exist is the same
 * kind of fact as which drivers come up, and belongs beside the roster that
 * says so. The drive a path reaches is not listed here: it is one symbol,
 * drive_backend, and the host that is linked defines it.
 */

#include "host.h"

#include "core/api/dir.h"
#include "core/api/std.h"
#include "core/api/fs.h"
#include "ria/mon/rom.h"
#include "ria/net/modem.h"
#include "ria/usb/mid.h"
#include "ria/usb/nfc.h"
#include "ria/usb/vcp.h"

// Driver table, the filesystem is catch-all and must be last.
static HOST_IN_FLASH("std_drivers") const std_driver_t std_driver_table[] = {
    {modem_std_handles, modem_std_open, modem_std_close, modem_std_read, modem_std_write, NULL, NULL},
    {vcp_std_handles, vcp_std_open, vcp_std_close, vcp_std_read, vcp_std_write, NULL, NULL},
    {mid_std_handles, mid_std_open, mid_std_close, mid_std_read, mid_std_write, mid_std_sync, NULL},
    {rom_std_handles, rom_std_open, rom_std_close, rom_std_read, NULL, NULL, rom_std_lseek},
    {nfc_std_handles, nfc_std_open, nfc_std_close, nfc_std_read, nfc_std_write, NULL, NULL},
    {fs_std_handles, fs_std_open, fs_std_close, fs_std_read, fs_std_write, fs_std_sync, fs_std_lseek},
};

const std_driver_t *std_drivers(size_t *count)
{
    *count = sizeof(std_driver_table) / sizeof(std_driver_table[0]);
    return std_driver_table;
}
