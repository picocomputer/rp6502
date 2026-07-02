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

#ifndef _EMU_HOST_FAT_H_
#define _EMU_HOST_FAT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

void host_fat_disk_reset(void);    /* wipe the RAM disk to an unformatted state */
bool host_fat_mount(void);   /* --tmpdrive: format + mount a fresh RAM FatFs, make it the backend */
void host_fat_unmount(void); /* restore the native host backend (tests; the drive is session-lived otherwise) */
bool host_fat_active(void);       /* true once the FatFs backend is the active MSC0: drive */

/* The FatFs backend runs the SHARED ria/api/fat.c file driver (fat_std_*), listed
 * in std.c's table and gated on host_fat_active(); the dir syscalls run the
 * firmware's dir_api_* (ria/api/dir.c), swapped in via emu_dir_ops_set(). */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_FAT_H_ */
