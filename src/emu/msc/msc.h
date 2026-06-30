/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MSC0: the writable host filesystem driver (msc.c) — the catch-all entry in
 * std.c's stdio driver table. open() hands back an opaque per-file descriptor
 * the cached read/write/lseek/close/sync act on. Internal wiring across the fs
 * backends, not for general callers.
 */

#ifndef _EMU_MSC_H_
#define _EMU_MSC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emu/api/api.h" /* io_result */

#ifdef __cplusplus
extern "C"
{
#endif

/* Enable POSIX AIO for data transfers (the windowed real-time loop). Off by
 * default: headless/tests and the web build do synchronous I/O. */
void msc_set_async(bool on);

bool msc_std_handles(const char *path);
void *msc_std_open(const char *path, uint8_t flags); /* desc, or NULL + errno */
void msc_std_close(void *desc);
io_result msc_std_read(void *desc, void *buf, size_t n, size_t *got);
io_result msc_std_write(void *desc, const void *buf, size_t n, size_t *put);
void msc_std_sync(void *desc); /* persist the drive (web: IDBFS) */
long msc_std_lseek(void *desc, long off, int whence);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_MSC_H_ */
