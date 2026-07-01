/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_FAT_H_
#define _RIA_API_FAT_H_

/* The FatFs filesystem backend: the stdio file driver (open/close/read/write/
 * lseek/sync over a FIL pool) plus the directory/metadata ops (stat/opendir/…)
 * over a DIR pool. FatFs-only — the block device (diskio) is supplied by the
 * platform (usb/msc.c on hardware, a RAM disk in the emulator), so this file is
 * shared: the firmware calls these directly and the emulator plugs them into its
 * runtime-swappable OS vtables. Every op returns 0/-1 with *err on failure.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "api/api.h"
#include "api/std.h"
#include "fatfs/ff.h"

/* File driver (std.c's catch-all entry). */
bool fat_std_handles(const char *path);
int fat_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result fat_std_close(int desc, api_errno *err);
std_rw_result fat_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result fat_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);
int fat_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err);
std_rw_result fat_std_sync(int desc, api_errno *err);

/* Directory / metadata ops. The DIR pool lifecycle (machine run/stop). */
void fat_dir_run(void);
void fat_dir_stop(void);

int fat_stat(const char *path, FILINFO *fno, api_errno *err);
int fat_opendir(const char *path, api_errno *err); /* >=0 descriptor, or -1 */
int fat_readdir(int des, FILINFO *fno, api_errno *err); /* fname[0]==0 at end */
int fat_closedir(int des, api_errno *err);
int fat_telldir(int des, int32_t *pos, api_errno *err);
int fat_seekdir(int des, int32_t offs, api_errno *err);
int fat_rewinddir(int des, api_errno *err);
int fat_unlink(const char *path, api_errno *err);
int fat_rename(const char *oldp, const char *newp, api_errno *err);
int fat_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err);
int fat_utime(const char *path, const FILINFO *fno, api_errno *err);
int fat_mkdir(const char *path, api_errno *err);
int fat_chdir(const char *path, api_errno *err);
int fat_chdrive(const char *path, api_errno *err);
int fat_getcwd(char *buf, size_t size, api_errno *err);
int fat_getlabel(const char *path, char *label, api_errno *err); /* label >= 12 bytes */
int fat_setlabel(const char *path, api_errno *err);
int fat_getfree(const char *path, uint32_t *free_sectors, uint32_t *total_sectors, api_errno *err);

#endif /* _RIA_API_FAT_H_ */
