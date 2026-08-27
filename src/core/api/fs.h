/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The drive a 6502 reaches when a host has a real filesystem underneath it:
 * the std driver for its files, the backend for core/api/dir.c's directory
 * syscalls, and the address translation between the two spellings of a path.
 *
 * Everything below it is host/api/fs.h and host/api/dir.h, so this is the same
 * file on every software machine and the host is what changes.
 */

#ifndef _CORE_API_FS_H_
#define _CORE_API_FS_H_

#include <stdbool.h>
#include <stddef.h>

#include "core/api/dir.h"
#include "core/api/std.h"
#include "host/api/fs.h"

#define FS_MAX_PATH 4096 /* host path buffer size for fs_to_host callers */

/* Path addressing: "MSC0:/x" native "/x", "MSC0:x" the cwd, "MSC0://C/x" a
 * Windows drive. */
bool fs_to_host(const char *path, char *host, size_t hsz);          /* MSC0: -> host path */
size_t fs_from_host(const char *hostpath, char *out, size_t outsz); /* host -> MSC0: */
bool fs_has_drive_prefix(const char *path);   /* path carries an MSC0:/N: prefix */
const char *fs_strip_drive(const char *path); /* path past a recognized drive prefix */

/* Convert a host (POSIX) errno to an api_errno. */
api_errno fs_errno_to_api_errno(int host_errno);

/* Convert a host transfer outcome to the guest stdio driver's. The two enums say
 * the same three things and are deliberately separate types, so the crossing is
 * written down rather than assumed to be a cast. */
std_rw_result fs_io_to_std_result(host_io_result r);

/* The files, for std.c's driver table (the writable catch-all, registered last). */
bool fs_std_handles(const char *path);
int fs_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result fs_std_close(int desc, api_errno *err);
std_rw_result fs_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err);
std_rw_result fs_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err);
std_rw_result fs_std_sync(int desc, api_errno *err);
int fs_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err);

/* This drive, for core/api/dir.c's handlers. */
extern const dir_backend_t fs_dir_backend;

#endif /* _CORE_API_FS_H_ */
