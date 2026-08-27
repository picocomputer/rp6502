/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The catch-all entry in this machine's std driver table: files on the FAT
 * volume, over a FIL pool. See api/dir.h for the same volume's directories.
 */

#ifndef _RIA_API_FS_H_
#define _RIA_API_FS_H_

#include "core/api/api.h"
#include "core/api/std.h"
#include <stdbool.h>
#include <stdint.h>

bool fat_std_handles(const char *path);
int fat_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result fat_std_close(int desc, api_errno *err);
std_rw_result fat_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result fat_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);
int fat_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err);
std_rw_result fat_std_sync(int desc, api_errno *err);

#endif /* _RIA_API_FS_H_ */
