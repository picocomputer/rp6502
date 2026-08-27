/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The drive core/api/dir.c asks for, on a machine whose storage is a host
 * filesystem: the directory syscalls answered through host/api/fs.h and
 * host/api/dir.h, and the FILINFO the 6502 expects synthesized from what an
 * OS actually keeps.
 *
 * A machine with FAT underneath answers the same questions natively and does
 * not compile this -- see host/pico/ria/api/dir.c. core/api/fs.c, the files
 * beside these directories, is the same on both.
 */

#ifndef _CORE_API_DRIVE_H_
#define _CORE_API_DRIVE_H_

#include "core/api/dir.h"

extern const dir_backend_t drive_backend;

#endif /* _CORE_API_DRIVE_H_ */
