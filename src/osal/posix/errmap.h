/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* POSIX errno, in the API's words.
 */

#ifndef _OSAL_POSIX_ERRMAP_H_
#define _OSAL_POSIX_ERRMAP_H_

#include "core/api/api.h"

api_errno errno_to_api(int host_errno);

/* The same mapping, for a read or a write. The descriptor is always one this
 * driver handed out and still open, core/api/std.c having checked the guest's and
 * the ROM readers using fs_rom_open's own, so an EBADF from the OS here can
 * only mean the file was opened for the other direction, which every other
 * machine reports as EACCES. */
api_errno errno_to_api_rw(int host_errno);

#endif /* _OSAL_POSIX_ERRMAP_H_ */
