/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The files on this machine's drive, as the 6502 opens them: the catch-all
 * entry in std.c's driver table.
 *
 * This is a contract and nothing else. Every machine's drivers.c lists these
 * seven, and the host that implements them is what changes -- host/posix/fs.c,
 * host/windows/fs.c, host/emsdk/fs.c, host/pico/ria/api/fs.c,
 * host/pocket/sw/fs.c. Linking a different one is the whole of the seam.
 *
 * The directories beside these files are core/api/dir.c's, over a drive each
 * host supplies the same way (core/api/dir.h's drive_* calls).
 */

#ifndef _CORE_API_FS_H_
#define _CORE_API_FS_H_

#include "core/api/std.h"
#include <stdbool.h>
#include <stdint.h>

/* The SDK's open flags, as a program passes them. The low two bits mirror
 * FatFs FA_READ/FA_WRITE on purpose, so a FAT machine passes the access mode
 * straight through; a host with POSIX translates. */
#define FS_RD 0x01
#define FS_WR 0x02
#define FS_CREAT 0x10
#define FS_TRUNC 0x20
#define FS_APPEND 0x40
#define FS_EXCL 0x80

/* The std driver's slots. Signatures are std_driver_t's, because that is what
 * they are. */
bool fs_std_handles(const char *path);
int fs_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result fs_std_close(int desc, api_errno *err);
std_rw_result fs_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err);
std_rw_result fs_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err);
std_rw_result fs_std_sync(int desc, api_errno *err);
int fs_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err);

/* Read-only, for the ROM loader alone: the .rp6502 a machine is running out
 * of. One at a time -- a second open replaces the first, which is rom_load's
 * shape -- and the descriptor is outside the range fs_std_open can return, so
 * a program can neither reach it nor collide with it. fs_std_read, lseek and
 * close accept it, so a window needs nothing else.
 *
 * A program opening the same file by name gets an ordinary descriptor from
 * fs_std_open, and the two are unrelated. */
int fs_rom_open(const char *path, api_errno *err);

/* This driver's stdio row: the std_driver_t initializer core/api/std.c
 * builds this machine's table from. The catch-all: a machine lists it last, after every driver that claims a
 * name of its own. */
#define FS_STD_LIFECYCLE           \
    {                              \
        .handles = fs_std_handles, \
        .open = fs_std_open,       \
        .close = fs_std_close,     \
        .read = fs_std_read,       \
        .write = fs_std_write,     \
        .sync = fs_std_sync,       \
        .lseek = fs_std_lseek,     \
    }

#endif /* _CORE_API_FS_H_ */
