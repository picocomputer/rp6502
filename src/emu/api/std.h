/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The stdio open dispatcher (std.c): the host-side file API over std.c's driver
 * table (the stdio open dispatcher), plus the per-frame line-editor pump and the
 * machine-reset hooks. The std_api_* / dir_api_* 6502 syscall handlers are
 * declared by the firmware's api/std.h + api/dir.h, which std.c/dir.c include
 * directly; only the emu-core entry points live here.
 */

#ifndef _EMU_STD_H_
#define _EMU_STD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emu/api/api.h" /* io_result */

#ifdef __cplusplus
extern "C"
{
#endif

/* ---- Host-side file API over std.c's driver table (the stdio open dispatcher).
 * The 6502 reaches it through the std_api_* syscalls; main.c/tests use it here.
 * An fd is a small int into the std fd pool; flags are the SDK open() bits. ---- */
int std_open(const char *path, uint8_t flags); /* fd >= 0, or -1 + errno */
bool std_writable(int fd);
io_result std_read(int fd, void *buf, size_t n, size_t *got);
io_result std_write(int fd, const void *buf, size_t n, size_t *put);
long std_lseek(int fd, long offset, int whence); /* -1 on error */
void std_close(int fd);

void std_task(void);        /* pump the line editor (rln) once per frame */
void std_reset(void);       /* machine reset: close open files + dirs, reset the console */
void std_files_reset(void); /* close every open file fd (driver close frees its object) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_STD_H_ */
