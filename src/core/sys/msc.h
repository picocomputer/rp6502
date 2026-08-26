/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_SYS_MSC_H_
#define _CORE_SYS_MSC_H_

#include <stdbool.h>
#include <stddef.h>

#include "core/api/dir.h"
#include "core/api/std.h"
#include "host/fs.h"

#define MSC_MAX_PATH 4096 /* host path buffer size for msc_to_host callers */

/* Path addressing: "MSC0:/x" native "/x", "MSC0:x" the cwd, "MSC0://C/x" a
 * Windows drive. */
bool msc_to_host(const char *path, char *host, size_t hsz);          /* MSC0: -> host path */
size_t msc_from_host(const char *hostpath, char *out, size_t outsz); /* host -> MSC0: */
bool msc_has_drive_prefix(const char *path);   /* path carries an MSC0:/N: prefix */
const char *msc_strip_drive(const char *path); /* path past a recognized drive prefix */

/* Convert a host (POSIX) errno to an api_errno. */
api_errno msc_errno_to_api_errno(int host_errno);

/* Convert a host transfer outcome to the guest stdio driver's. The two enums say
 * the same three things and are deliberately separate types, so the crossing is
 * written down rather than assumed to be a cast. */
std_rw_result msc_io_to_std_result(fs_io_result r);

/* The native host MSC0: file driver (the writable catch-all), for std.c's table. */
bool msc_std_handles(const char *path);
int msc_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result msc_std_close(int desc, api_errno *err);
std_rw_result msc_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err);
std_rw_result msc_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err);
std_rw_result msc_std_sync(int desc, api_errno *err);
int msc_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err);

/* This drive, for core/api/dir.c's handlers. */
extern const dir_backend_t msc_dir_backend;

#endif /* _CORE_SYS_MSC_H_ */
