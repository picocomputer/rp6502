/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * POSIX AIO headers, pulled in only where the platform provides it (EMU_HAVE_AIO,
 * set by CMake's aio_read probe). Callers gate their aiocb code on the same macro
 * and fall back to synchronous fs_* I/O otherwise.
 */

#ifndef _EMU_HOST_AIO_H_
#define _EMU_HOST_AIO_H_

#ifdef EMU_HAVE_AIO
#include <aio.h>
#include <unistd.h>
#endif

#endif /* _EMU_HOST_AIO_H_ */
