/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The files on this machine's drive, as the 6502 opens them: the catch-all
 * entry in std.c's driver table.
 *
 * This is a contract and nothing else. Every machine's drivers.h names the
 * row below, and the host that implements the seven is what changes -- osal/posix/fs.c,
 * osal/windows/fs.c, host/itch.io/fs.c, host/pico/ria/api/fs.c,
 * host/pocket/sw/fs.c. Linking a different one is the whole of the seam.
 *
 * The directories beside these files are core/api/dir.c's, over a drive each
 * host supplies the same way (osal/dir.h's drive_* calls).
 */

#ifndef _OSAL_FS_H_
#define _OSAL_FS_H_

#include "core/api/std.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h> /* FILE, for the ROM loader's stream below */

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

/* The machine's own opens: the .rp6502 a loader streams. On the machine that
 * stores installed ROMs itself -- littlefs on a Pico -- ":name" opens one
 * from the store, ENOENT on a miss, and never falls through to the
 * filesystem, so an install cannot shadow a file. A machine whose installs
 * are references keeps that map in the loader (rom/alias.c) and no colon
 * ever reaches here. No name policy lives anywhere in this: the contents
 * make a ROM, and every caller reads the shebang right after open.
 *
 * FS_RD opens the one read descriptor; the caller closes it before opening
 * again -- a clean lifecycle that lints bugs, so nothing here closes an old
 * one behind the caller's back. FS_WR|FS_CREAT|FS_EXCL, and only that, and
 * only on ":name", is INSTALL's one write -- EACCES where the store holds
 * references or nothing. Any other flags are EINVAL.
 *
 * The descriptors are outside the range fs_std_open can return, so a program
 * can neither reach one nor be handed one; fs_std_read, write, lseek and
 * close accept them, so a caller needs nothing else. */
int fs_rom_open(const char *path, uint8_t flags, api_errno *err);

/* Take ":name" off the null drive: REMOVE, and INSTALL cleaning up a write
 * that failed. */
bool fs_rom_remove(const char *name, api_errno *err);

/* The two filesystem calls core makes directly rather than through a driver.
 * A path is spelled the way the 6502 spells it, both ways.
 *
 * realpath answers absolutely, which is what argv[0] needs to survive a chdir;
 * fopen_rd hands back a stream for the ROM loader's record parser, which reads
 * a whole file rather than serving a program.
 *
 * realpath allocates its answer, because how long a path the OS will hand back
 * is the OS's to decide and not a caller's to guess. The caller frees; NULL is
 * a path that does not resolve. */
char *os_fs_realpath(const char *path);
FILE *os_fs_fopen_rd(const char *path);

/* This driver's stdio row: the std_driver_t initializer core/api/std.c
 * builds this machine's table from. The catch-all: a machine lists it last, after every driver that claims a
 * name of its own. */
#define FS_STD_DRIVER           \
    {                              \
        .handles = fs_std_handles, \
        .open = fs_std_open,       \
        .close = fs_std_close,     \
        .read = fs_std_read,       \
        .write = fs_std_write,     \
        .sync = fs_std_sync,       \
        .lseek = fs_std_lseek,     \
    }

#endif /* _OSAL_FS_H_ */
