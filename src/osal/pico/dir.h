/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_PICO_DIR_H_
#define _OSAL_PICO_DIR_H_

#include "core/api/api.h"
#include <stdbool.h>

/* FatFs takes the first ':' anywhere in a path as the end of a drive name, and
 * a control character as the end of the path, so a path from a program is
 * checked against the rules of path_fat_ok in core/str/path.h before any
 * FatFs call. */
bool fat_path_ok(const char *path, api_errno *err);

/* True when path opens as a directory, a root included. An empty path and a
 * bare drive name, which f_opendir takes as the current folder, give false. */
bool fat_names_dir(const char *path);

#endif /* _OSAL_PICO_DIR_H_ */
