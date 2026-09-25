/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_FS_H_
#define _OSAL_FS_H_

#include "core/api/save.h"
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

/* A host path, as opposed to a drive path, is in the host's syntax and in
 * UTF-8 whatever the code page, and the FAT name rules do not apply to it,
 * because a program never passes one. An installed ROM is one of these
 * (core/rom/alias.c). fs_host_realpath returns the absolute form of a file or
 * folder that exists, allocated for the caller to free, or NULL.
 * fs_rom_open_host opens a ROM image for reading as fs_rom_open does, in the
 * same descriptor space. */
char *fs_host_realpath(const char *host);
int fs_rom_open_host(const char *host, api_errno *err);

/* fs_save_start sets the SAVE: folder each time a program starts: the
 * host_save_dir of host/host.h, or the working directory at that moment when
 * that is NULL. fs_save_open opens name in that folder, so a CHDIR or CHDRIVE
 * by the program does not move its saves. save_std_open has already checked
 * name against the rules in core/api/save.h. The descriptor is one the
 * fs_std_ functions accept, and fs_std_ident records it as SAVE:name, so a
 * savestate holds no host path for it and fs_std_reopen opens it again
 * through save_std_open. The folder is kept after a stop, because a savestate
 * loaded into a stopped machine still reopens its SAVE: files there. A host
 * that unloads this library frees it with fs_save_free. */
int fs_save_open(const char *name, uint8_t flags, api_errno *err);
void fs_save_start(void);
void fs_save_free(void);

#define SAVE_STD_DRIVER              \
    {                                \
        .handles = save_std_handles, \
        .open = save_std_open,       \
        .close = fs_std_close,       \
        .read = fs_std_read,         \
        .write = fs_std_write,       \
        .sync = fs_std_sync,         \
        .lseek = fs_std_lseek,       \
        .ident = fs_std_ident,       \
        .reopen = fs_std_reopen,     \
    }

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
