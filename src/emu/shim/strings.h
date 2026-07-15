/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MSVC lacks POSIX <strings.h>; map the case-insensitive compares the
 * shared firmware sources need.
 */

#ifndef _EMU_SHIM_STRINGS_H_
#define _EMU_SHIM_STRINGS_H_

#include <string.h>

#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include_next <strings.h>
#endif

#endif /* _EMU_SHIM_STRINGS_H_ */
