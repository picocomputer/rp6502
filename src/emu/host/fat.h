/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
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

void hostfat_disk_reset(void);    /* wipe the RAM disk to an unformatted state */
bool hostfat_mount(void);   /* --tmpdrive: format + mount a fresh RAM FatFs, make it the backend */
void hostfat_unmount(void); /* restore the native host backend (tests; the drive is session-lived otherwise) */
bool hostfat_active(void);       /* true once the FatFs backend is the active MSC0: drive */

/* The FatFs backend runs the SHARED ria/api/fat.c file driver (fat_std_*), listed
 * in std.c's table and gated on hostfat_active(); the dir syscalls run the
 * firmware's dir_api_* (ria/api/dir.c), swapped in via hostdir_ops_set(). */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_FAT_H_ */
