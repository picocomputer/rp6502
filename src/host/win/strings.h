/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MSVC lacks POSIX <strings.h>; map the case-insensitive compares the
 * shared firmware sources need. This directory is on the include path only
 * under MSVC, so every other host resolves <strings.h> to its own.
 */

#ifndef _EMU_HOST_WIN_STRINGS_H_
#define _EMU_HOST_WIN_STRINGS_H_

#include <string.h>

#define strcasecmp _stricmp
#define strncasecmp _strnicmp

#endif /* _EMU_HOST_WIN_STRINGS_H_ */
