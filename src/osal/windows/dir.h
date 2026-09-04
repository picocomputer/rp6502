/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* A path as the 6502 spells it, and back, for the files beside this one.
 *
 * Here rather than beside the opens because resolving a path is what a
 * directory is for: the drive prefix these take off and put back names the
 * one directory this drive is, and the cwd a relative path is resolved
 * against is drive_getcwd's.
 */

#ifndef _OSAL_WINDOWS_DIR_H_
#define _OSAL_WINDOWS_DIR_H_

#include "core/api/api.h"
#include <wchar.h>

/* Both allocate to fit -- path_to_native never grows a path, and one UTF-16
 * unit per OEM byte answers the other way -- so neither caps and neither
 * guesses. The caller frees. NULL sets *err. */
wchar_t *path_to_wide(const char *path, api_errno *err);
char *path_from_wide(const wchar_t *w, api_errno *err);

/* Rewrite '\\' to '/' in place (the 6502's paths are '/'-separated). */
void win_to_slash(char *p);

#endif /* _OSAL_WINDOWS_DIR_H_ */
