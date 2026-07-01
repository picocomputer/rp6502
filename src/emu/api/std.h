/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The stdio open dispatcher (std.c): the file-driver interface, the host-side
 * file API over std.c's driver table, the per-frame line-editor pump, and the
 * machine-reset hooks. The std_api_* / dir_api_* 6502 syscall handlers are
 * declared by the firmware's api/std.h + api/dir.h, which std.c/dir.c include
 * directly; only the emu-core entry points live here.
 */

#ifndef _EMU_STD_H_
#define _EMU_STD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "api/api.h" /* api_errno */
#include "api/std.h" /* std_rw_result */

#ifdef __cplusplus
extern "C"
{
#endif

/* One stdio file driver (matches the firmware ria/api/std.c std_driver_t): a path
 * is claimed by the first driver whose handles() returns true; open() hands back a
 * small int descriptor the rest act on. read/write/close/sync return std_rw_result
 * (STD_PENDING re-polls an async transfer); every op reports failure via *err. */
typedef struct
{
    bool (*handles)(const char *path);
    int (*open)(const char *path, uint8_t flags, api_errno *err); /* desc >= 0, or -1 */
    std_rw_result (*close)(int desc, api_errno *err);
    std_rw_result (*read)(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err);
    std_rw_result (*write)(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err);
    std_rw_result (*sync)(int desc, api_errno *err);
    int (*lseek)(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err);
} std_driver_t;

/* The active writable-filesystem driver (std.c's catch-all), swapped at runtime by
 * the drive lifecycle: host_file_driver by default, fat_file_driver on --tmpdrive. */
void emu_set_fs_driver(const std_driver_t *drv);

/* ---- Host-side file API over std.c's driver table (the stdio open dispatcher).
 * The 6502 reaches it through the std_api_* syscalls; main.c/tests use it here.
 * An fd is a small int into the std fd pool; flags are the SDK open() bits. *err
 * (nullable) carries the failure code. ---- */
int std_open(const char *path, uint8_t flags, api_errno *err); /* fd >= 0, or -1 */
bool std_writable(int fd);
std_rw_result std_read(int fd, char *buf, uint32_t n, uint32_t *got, api_errno *err);
std_rw_result std_write(int fd, const char *buf, uint32_t n, uint32_t *put, api_errno *err);
long std_lseek(int fd, long offset, int whence); /* new position, or -1 on error */
void std_close(int fd);

void std_task(void);        /* pump the line editor (rln) once per frame */
void std_reset(void);       /* machine reset: close open files + dirs, reset the console */
void std_files_reset(void); /* close every open file fd (driver close frees its object) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_STD_H_ */
