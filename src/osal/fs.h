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

/* Leaves no transfer in flight, which a savestate needs because it reads and
 * writes the same memory a transfer is landing in. The host calls this before
 * it saves and before it loads. The transfer is cancelled and reaped rather
 * than allowed to finish, because finishing would advance the descriptor's
 * offset past bytes the 6502 never received, and the read that is dispatched
 * again after the load would start beyond them. A synchronous transport has
 * nothing in flight to cancel. */
void fs_std_settle(void);

/* Save a descriptor and open it again, in core/api/std.h's shape. The name is
 * made absolute when the file is opened, because both the guest and this core
 * may chdir before the save. */
#define FS_PATH_SLOT (API_PATH_MAX + 1)
bool fs_std_ident(int desc, sst_cursor_t *c);
int fs_std_reopen(sst_cursor_t *c, api_errno *err);

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
