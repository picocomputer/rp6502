/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Turns a path as the 6502 spells it into one the libc calls take. It lives
 * with the directory calls because the drive name it takes off is the one
 * this drive answers to, and a relative path resolves against the working
 * directory drive_getcwd reports.
 */

#ifndef _OSAL_POSIX_DIR_H_
#define _OSAL_POSIX_DIR_H_

#include "core/api/api.h"

/* The result is allocated to fit, because oem_to_utf8 answers how much room
 * it wants, and the caller frees it. A NULL return has set *err.
 *
 * There is no call going the other way: what this host answers with is
 * already a native path, so only the code page has to change, and the two
 * calls that answer a path do that themselves. */
char *path_to_utf8(const char *path, api_errno *err);

#endif /* _OSAL_POSIX_DIR_H_ */
