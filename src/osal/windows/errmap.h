/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The mapping is direct rather than by way of the CRT's errno, because a code
 * the CRT does not recognize becomes a generic errno, and what it was cannot
 * be recovered from that.
 *
 * The file is errmap.h and not errno.h, because a header of that name on an
 * include path is the one a translation unit gets instead of the C library's.
 */

#ifndef _OSAL_WINDOWS_ERRMAP_H_
#define _OSAL_WINDOWS_ERRMAP_H_

#include "core/api/api.h"
#include <windows.h>

api_errno win_error_to_api(DWORD e);

static inline api_errno win_last_error_to_api(void) { return win_error_to_api(GetLastError()); }

#endif /* _OSAL_WINDOWS_ERRMAP_H_ */
