/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_WINDOWS_DIR_H_
#define _OSAL_WINDOWS_DIR_H_

#include "core/api/api.h"
#include <wchar.h>

/* Both allocate to fit, because there is one UTF-16 unit per OEM byte and at
 * most one OEM byte per unit back. The caller frees. NULL sets *err. path_to_wide
 * applies the FAT name rules, so it takes only a drive path. */
wchar_t *path_to_wide(const char *path, api_errno *err);
char *path_from_wide(const wchar_t *w, api_errno *err);

void win_to_slash(char *p);

/* A host path is passed in UTF-8 and is not checked against the name rules.
 * Both allocate, and the caller frees. */
wchar_t *win_utf8_to_wide(const char *u8, api_errno *err);
char *win_wide_to_utf8(const wchar_t *w);

/* The absolute form Win32 gives a path, allocated. NULL sets *err. */
wchar_t *win_full_path(const wchar_t *w, api_errno *err);

/* Creates each missing folder that holds path. The path is cut and restored
 * in place. */
void win_make_parents(wchar_t *path);

#endif /* _OSAL_WINDOWS_DIR_H_ */
