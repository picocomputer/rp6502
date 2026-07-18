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

#define MSC_MAX_PATH 4096 /* host path buffer size for msc_to_host callers */

/* Path addressing: "MSC0:/x" native "/x", "MSC0:x" the cwd, "MSC0://C/x" a
 * Windows drive. */
bool msc_to_host(const char *path, char *host, size_t hsz);          /* MSC0: -> host path */
size_t msc_from_host(const char *hostpath, char *out, size_t outsz); /* host -> MSC0: */
bool msc_has_drive_prefix(const char *path);   /* path carries an MSC0:/N: prefix */
const char *msc_strip_drive(const char *path); /* path past a recognized drive prefix */

/* Convert a host (POSIX) errno to an api_errno. */
api_errno msc_errno_to_api_errno(int host_errno);

/* The native host MSC0: file driver (the writable catch-all), for std.c's table. */
bool msc_std_handles(const char *path);
int msc_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result msc_std_close(int desc, api_errno *err);
std_rw_result msc_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err);
std_rw_result msc_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err);
std_rw_result msc_std_sync(int desc, api_errno *err);
int msc_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err);

/* The native host MSC0: file/directory management syscall handlers, installed in
 * the OP array (the counterparts to the firmware's fat_api_*). */
bool msc_api_stat(void);
bool msc_api_opendir(void);
bool msc_api_readdir(void);
bool msc_api_closedir(void);
bool msc_api_telldir(void);
bool msc_api_seekdir(void);
bool msc_api_rewinddir(void);
bool msc_api_unlink(void);
bool msc_api_rename(void);
bool msc_api_chmod(void);
bool msc_api_utime(void);
bool msc_api_mkdir(void);
bool msc_api_chdir(void);
bool msc_api_chdrive(void);
bool msc_api_getcwd(void);
bool msc_api_setlabel(void);
bool msc_api_getlabel(void);
bool msc_api_getfree(void);

void msc_stop(void); /* close open host directories (machine reset) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_MSC_H_ */
