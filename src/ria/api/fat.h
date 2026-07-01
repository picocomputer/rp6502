/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_FAT_H_
#define _RIA_API_FAT_H_

/* The FatFs stdio file driver: the catch-all filesystem entry in std.c's driver
 * table (open/close/read/write/lseek/sync over a FIL pool). FatFs-only — the block
 * device (diskio) is supplied by the platform (usb/msc.c on hardware, a RAM disk
 * in the emulator), so this file is shared with the emulator. The directory API
 * is ria/api/dir.c (its own f_*), which the emulator reuses directly.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "api/api.h"
#include "api/std.h"

bool fat_std_handles(const char *path);
int fat_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result fat_std_close(int desc, api_errno *err);
std_rw_result fat_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result fat_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);
int fat_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err);
std_rw_result fat_std_sync(int desc, api_errno *err);

#endif /* _RIA_API_FAT_H_ */
