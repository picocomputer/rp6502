/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/std.h"
#include "api/fat.h"
#include "mon/rom.h"
#include "net/mdm.h"
#include "usb/mid.h"
#include "usb/nfc.h"
#include "usb/vcp.h"
#include <pico/stdlib.h>

// Driver table, msc is catch-all and must be last.
__in_flash("std_drivers") const std_driver_t std_drivers[] = {
    {mdm_std_handles, mdm_std_open, mdm_std_close, mdm_std_read, mdm_std_write, NULL, NULL},
    {vcp_std_handles, vcp_std_open, vcp_std_close, vcp_std_read, vcp_std_write, NULL, NULL},
    {mid_std_handles, mid_std_open, mid_std_close, mid_std_read, mid_std_write, mid_std_sync, NULL},
    {rom_std_handles, rom_std_open, rom_std_close, rom_std_read, NULL, NULL, rom_std_lseek},
    {nfc_std_handles, nfc_std_open, nfc_std_close, nfc_std_read, nfc_std_write, NULL, NULL},
    {fat_std_handles, fat_std_open, fat_std_close, fat_std_read, fat_std_write, fat_std_sync, fat_std_lseek},
};

const size_t std_driver_count = sizeof(std_drivers) / sizeof(std_drivers[0]);
