/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator's FatFs backend for MSC0: — a RAM block device (the diskio, called
 * by FatFs) plus the drive glue that lets --tmpdrive present a real, ephemeral
 * FatFs instead of the native host filesystem. The 6502's file/dir syscalls route
 * here (via std.c / host/dir.c) when the FatFs backend is active, running the
 * SHARED ria/api/fat.c driver over the RAM disk — the same code as the firmware.
 */

#ifndef _EMU_USB_MSC_H_
#define _EMU_USB_MSC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emu/api/std.h"  /* std_driver_t */
#include "emu/host/dir.h" /* fs_dir_ops */

#ifdef __cplusplus
extern "C"
{
#endif

void emu_ramdisk_reset(void);    /* wipe the RAM disk to an unformatted state */
bool emu_ramdrive_mount(void);   /* --tmpdrive: format + mount a fresh RAM FatFs, make it the backend */
void emu_ramdrive_unmount(void); /* restore the native host backend (tests; the drive is session-lived otherwise) */
bool emu_fat_active(void);       /* true once the FatFs backend is the active MSC0: drive */

/* The FatFs backend for MSC0:, plugged into the emu's runtime dir vtable + std.c
 * catch-all driver — both the shared ria/api/fat.c ops directly, no adapter. */
extern const fs_dir_ops fat_dir_ops;
extern const std_driver_t fat_file_driver;

#ifdef __cplusplus
}
#endif

#endif /* _EMU_USB_MSC_H_ */
