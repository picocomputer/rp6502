/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What went wrong, in the API's words. Win32 says it with GetLastError, so
 * that is what this maps -- straight there, rather than through POSIX errno
 * on the way. Two tables in series only lose whatever the middle one cannot
 * spell.
 *
 * The two path scraps every Win32 file here needs come along, because they
 * are too small to be a file and there is nowhere else both fs.c and dir.c
 * already look.
 *
 * Named errmap and not errno: this maps one vocabulary onto another, which is
 * not what a C library's errno.h is, and a header of that name anywhere a
 * build might put on an include path is the one a translation unit picks up
 * instead of its own.
 */

#ifndef _OSAL_WINDOWS_ERRMAP_H_
#define _OSAL_WINDOWS_ERRMAP_H_

#include "core/api/api.h"
#include <windows.h>

/* Rewrite '\\' to '/' in place (the 6502's paths are '/'-separated). */
void win_to_slash(char *p);

api_errno win_error_to_api(DWORD e);

/* The last failure, mapped. */
static inline api_errno win_last_error_to_api(void) { return win_error_to_api(GetLastError()); }

#endif /* _OSAL_WINDOWS_ERRMAP_H_ */
