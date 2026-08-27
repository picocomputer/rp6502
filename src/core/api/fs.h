/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The files on this machine's drive, as the 6502 opens them: the catch-all
 * entry in std.c's driver table, over host/api/fs.h.
 *
 * There is one of these and every machine installs it -- the host underneath
 * is what changes, and that is the whole of the seam. The directories beside
 * these files are core/api/dir.c's, over a drive of their own.
 */

#ifndef _CORE_API_FS_H_
#define _CORE_API_FS_H_

#include <stdbool.h>
#include <stddef.h>

#include "core/mem.h"
#include "core/api/std.h"
#include "host/api/fs.h"

/* A buffer for a path the API carries. One arrives on the xstack and getcwd
 * writes one back there, so XSTACK_SIZE is the whole of what the API can
 * deliver; the extra byte is mem.c's always-zero end+1, which lets the 6502
 * push a full stack without a terminator of its own. A host path is not
 * bounded by this -- see host/api/fs.h's HOST_MAX_PATH. */
#define FS_MAX_PATH (XSTACK_SIZE + 1)

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

#endif /* _CORE_API_FS_H_ */
