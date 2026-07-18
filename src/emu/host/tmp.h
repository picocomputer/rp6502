/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_HOST_TMP_H_
#define _EMU_HOST_TMP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

void tmp_disk_reset(void); /* wipe the RAM disk to an unformatted state */
bool tmp_mount(void);      /* --tmpdrive: format + mount a fresh RAM FatFs, make it the backend */
void tmp_unmount(void);    /* restore the native host backend (tests; the drive is session-lived otherwise) */
bool tmp_active(void);     /* true once the FatFs backend is the active MSC0: drive */

/* The FatFs backend runs the SHARED ria/api/fat.c file driver (fat_std_*), listed
 * in std.c's table and gated on tmp_active(); the dir syscalls run the firmware's
 * fat_api_* (ria/api/fat.c), swapped in via main_dir_ops_set(). */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_TMP_H_ */
