/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_HOST_MSC_H_
#define _EMU_HOST_MSC_H_

#include <stdbool.h>
#include <stddef.h>

#include "emu/api/std.h" /* std_driver_t */

#ifdef __cplusplus
extern "C"
{
#endif

#define HOST_MSC_MAX_PATH 4096 /* host path buffer size for host_msc_to_host callers */

/* Path addressing: "MSC0:/x" native "/x", "MSC0:x" the cwd, "MSC0://C/x" a
 * Windows drive. */
bool host_msc_to_host(const char *path, char *host, size_t hsz);          /* MSC0: -> host path */
size_t host_msc_from_host(const char *hostpath, char *out, size_t outsz); /* host -> MSC0: */
bool host_msc_has_drive_prefix(const char *path);   /* path carries an MSC0:/N: prefix */
const char *host_msc_strip_drive(const char *path); /* path past a recognized drive prefix */

/* Enable POSIX AIO for data transfers (the windowed real-time loop). Off by
 * default: headless/tests and the web build do synchronous I/O. */
void host_msc_set_async(bool on);

/* Convert a host (POSIX) errno to an api_errno. */
api_errno host_msc_errno_to_api_errno(int host_errno);

/* The native host MSC0: file driver (the writable catch-all), for std.c's table. */
bool host_msc_std_handles(const char *path);
int host_msc_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result host_msc_std_close(int desc, api_errno *err);
std_rw_result host_msc_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err);
std_rw_result host_msc_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err);
std_rw_result host_msc_std_sync(int desc, api_errno *err);
int host_msc_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_MSC_H_ */
