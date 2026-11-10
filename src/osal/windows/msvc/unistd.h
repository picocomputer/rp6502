/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* MSVC has no POSIX <unistd.h>, so this defines the standard stream
 * descriptors the shared sources use. The directory is on the include path
 * only under MSVC, so every other host still resolves <unistd.h> to its own. */

#ifndef _OSAL_WINDOWS_MSVC_UNISTD_H_
#define _OSAL_WINDOWS_MSVC_UNISTD_H_

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#endif /* _OSAL_WINDOWS_MSVC_UNISTD_H_ */
