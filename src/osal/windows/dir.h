/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_WINDOWS_DIR_H_
#define _OSAL_WINDOWS_DIR_H_

#include "core/api/api.h"
#include <wchar.h>

/* Both allocate to fit, because there is one UTF-16 unit per OEM byte and one
 * OEM byte per unit back. The caller frees. NULL sets *err. */
wchar_t *path_to_wide(const char *path, api_errno *err);
char *path_from_wide(const wchar_t *w, api_errno *err);

void win_to_slash(char *p);

#endif /* _OSAL_WINDOWS_DIR_H_ */
