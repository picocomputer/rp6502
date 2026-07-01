/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MSC0: the writable host filesystem file driver (fs.c) — the catch-all entry in
 * std.c's stdio driver table. open() hands back a small int descriptor the cached
 * read/write/lseek/close/sync act on. Internal wiring across the fs backends, not
 * for general callers.
 */

#ifndef _EMU_HOST_FS_H_
#define _EMU_HOST_FS_H_

#include <stdbool.h>

#include "emu/api/std.h" /* std_driver_t */

#ifdef __cplusplus
extern "C"
{
#endif

/* Enable POSIX AIO for data transfers (the windowed real-time loop). Off by
 * default: headless/tests and the web build do synchronous I/O. */
void host_set_async(bool on);

extern const std_driver_t host_file_driver; /* the native host MSC0: file driver */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_FS_H_ */
