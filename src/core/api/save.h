/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_API_SAVE_H_
#define _CORE_API_SAVE_H_

/* SAVE: is one flat folder of saves, and each machine puts it where its host
 * keeps saved data. The name rules are the same on every machine, so a name
 * that opens on one opens on all of them. The host's fs_save_open
 * opens the file behind a name, and everything after the open is the
 * filesystem row's (osal/fs.h SAVE_STD_DRIVER). */

#include "core/api/api.h"
#include <stdbool.h>
#include <stdint.h>

#define SAVE_NAME_MAX 32

bool save_std_handles(const char *path);

/* path starts with SAVE: in any case. One separator after the colon is
 * dropped, so SAVE:/x and SAVE:x name the same file. A name outside the rules
 * fails with API_EINVAL. */
int save_std_open(const char *path, uint8_t flags, api_errno *err);

#endif /* _CORE_API_SAVE_H_ */
