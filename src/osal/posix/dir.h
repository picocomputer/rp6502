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

#ifndef _OSAL_POSIX_DIR_H_
#define _OSAL_POSIX_DIR_H_

#include "core/api/api.h"

/* Both allocate to fit -- path_to_native never grows a path and the code page
 * conversions answer how much room they want -- so neither caps and neither
 * guesses. The caller frees. NULL sets *err. */
char *path_to_utf8(const char *path, api_errno *err);
char *path_from_utf8(const char *u8, api_errno *err);

#endif /* _OSAL_POSIX_DIR_H_ */
