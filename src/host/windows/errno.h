/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What went wrong, in the API's words. Win32 says it with GetLastError, so
 * that is what this maps -- straight there, rather than through POSIX errno
 * on the way. Two tables in series only lose whatever the middle one cannot
 * spell.
 */

#ifndef _HOST_WINDOWS_ERRNO_H_
#define _HOST_WINDOWS_ERRNO_H_

#include "core/api/api.h"
#include <windows.h>

api_errno win_error_to_api(DWORD e);

/* The last failure, mapped. */
static inline api_errno win_last_error_to_api(void) { return win_error_to_api(GetLastError()); }

#endif /* _HOST_WINDOWS_ERRNO_H_ */
