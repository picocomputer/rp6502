/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_FAT_H_
#define _RIA_API_FAT_H_

/* The FatFs filesystem module: the stdio file driver (the catch-all entry in
 * std.c's driver table — open/close/read/write/lseek/sync over a FIL pool) and the
 * file/directory management API (the 0x1B..0x2E syscalls, over its own DIR pool).
 * FatFs-only — the block device (diskio) is supplied by the platform (usb/msc.c on
 * hardware, a RAM disk in the emulator), so this file is shared with the emulator.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ria/api/api.h"
#include "ria/api/std.h"

// Convert a FatFs FRESULT to an api_errno.
api_errno fat_fresult_to_api_errno(unsigned fresult);

bool fat_std_handles(const char *path);
int fat_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result fat_std_close(int desc, api_errno *err);
std_rw_result fat_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result fat_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);
int fat_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err);
std_rw_result fat_std_sync(int desc, api_errno *err);

// Main events
void fat_run(void);
void fat_stop(void);

// The file/directory management API implementations
bool fat_api_stat(void);
bool fat_api_opendir(void);
bool fat_api_readdir(void);
bool fat_api_closedir(void);
bool fat_api_telldir(void);
bool fat_api_seekdir(void);
bool fat_api_rewinddir(void);
bool fat_api_unlink(void);
bool fat_api_rename(void);
bool fat_api_chmod(void);
bool fat_api_utime(void);
bool fat_api_mkdir(void);
bool fat_api_chdir(void);
bool fat_api_chdrive(void);
bool fat_api_getcwd(void);
bool fat_api_setlabel(void);
bool fat_api_getlabel(void);
bool fat_api_getfree(void);

#endif /* _RIA_API_FAT_H_ */
