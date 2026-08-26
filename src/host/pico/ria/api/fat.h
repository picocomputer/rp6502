/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_FAT_H_
#define _RIA_API_FAT_H_

/* The FatFs filesystem module: the stdio file driver (the catch-all entry in
 * std.c's driver table — open/close/read/write/lseek/sync over a FIL pool) and
 * this machine's drive for the directory syscalls, over its own DIR pool.
 * FatFs-only — the block device (diskio) is usb/msc.c.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/api/api.h"
#include "core/api/dir.h"
#include "core/api/std.h"
#include "fatfs/ff.h"

// Convert a FatFs FRESULT to an api_errno.
api_errno fat_fresult_to_api_errno(unsigned fresult);

bool fat_std_handles(const char *path);
int fat_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result fat_std_close(int desc, api_errno *err);
std_rw_result fat_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result fat_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);
int fat_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err);
std_rw_result fat_std_sync(int desc, api_errno *err);

// This drive, for core/api/dir.c's handlers.
extern const dir_backend_t fat_dir_backend;

#endif /* _RIA_API_FAT_H_ */
