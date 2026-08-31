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
 * Named errmap and not errno because a host directory is on the include path
 * (mach/emu.cmake puts it there so host.h can be reached), and a header
 * called errno.h there is the one every C library picks up instead of its
 * own. Renaming it back breaks every translation unit at once.
 */

#ifndef _HOST_WINDOWS_ERRMAP_H_
#define _HOST_WINDOWS_ERRMAP_H_

#include "core/api/api.h"
#include <windows.h>

/* A wide path buffer. Win32's own limit is longer than the API can carry, so
 * this is generous rather than exact. */
#define WIN_WPATH_MAX 4096

/* Rewrite '\\' to '/' in place (the 6502's paths are '/'-separated). */
void win_to_slash(char *p);

api_errno win_error_to_api(DWORD e);

/* The last failure, mapped. */
static inline api_errno win_last_error_to_api(void) { return win_error_to_api(GetLastError()); }

#endif /* _HOST_WINDOWS_ERRMAP_H_ */
