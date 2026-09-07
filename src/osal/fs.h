/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_FS_H_
#define _OSAL_FS_H_

#include "core/api/std.h"
#include <stdbool.h>
#include <stdint.h>

#define FS_RD 0x01
#define FS_WR 0x02
#define FS_CREAT 0x10
#define FS_TRUNC 0x20
#define FS_APPEND 0x40
#define FS_EXCL 0x80

bool fs_std_handles(const char *path);
int fs_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result fs_std_close(int desc, api_errno *err);
std_rw_result fs_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err);
std_rw_result fs_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err);
std_rw_result fs_std_sync(int desc, api_errno *err);
int fs_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err);

/* Leave no transfer in flight. A savestate reads and writes the memory a
 * transfer is landing in, so the host calls this before it saves and before
 * it loads. Cancelled and reaped, never completed: a transfer allowed to
 * finish would advance the descriptor's offset while core's own position did
 * not, and the parked read must re-issue from where the blob puts it. A
 * synchronous transport has nothing to do here. */
void fs_std_settle(void);

/* What a descriptor is, and how to get it back. A savestate's two, in
 * core/api/std.h's shape. This drive writes the absolute name it was opened
 * by, the access it was opened for, and where it is now; the name is made
 * absolute at open time, because the guest may chdir and this core does too.
 * A drive that cannot say answers false, and a machine cannot be saved with a
 * file open on a drive that cannot say. */
#define FS_PATH_SLOT (API_PATH_MAX + 1)
bool fs_std_ident(int desc, sst_cursor_t *c);
int fs_std_reopen(sst_cursor_t *c, api_errno *err);

// File handle for ROM which the 6502 can not access dirfectly.
int fs_rom_open(const char *path, uint8_t flags, api_errno *err);
bool fs_rom_remove(const char *name, api_errno *err);

#define FS_STD_DRIVER           \
    {                              \
        .handles = fs_std_handles, \
        .open = fs_std_open,       \
        .close = fs_std_close,     \
        .read = fs_std_read,       \
        .write = fs_std_write,     \
        .sync = fs_std_sync,       \
        .lseek = fs_std_lseek,     \
        .ident = fs_std_ident,     \
        .reopen = fs_std_reopen,   \
    }

#endif /* _OSAL_FS_H_ */
