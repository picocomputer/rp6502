/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_STR_PATH_H_
#define _CORE_STR_PATH_H_

#include "core/api/api.h"
#include <stdbool.h>

/* True if c separates path components. These are the two FatFs accepts
 * (ff.c IsSeparator), which are also the two a Windows host path accepts. */
#define path_is_sep(c) ((c) == '/' || (c) == '\\')

const char *path_basename(const char *path);

/* The FAT rules for a path, so a host that accepts any name refuses what
 * FatFs refuses. after_drive is true when the caller has removed a drive name
 * that this machine has from the front of path. Otherwise a ':' before the
 * first separator ends the name of a drive the machine does not have,
 * API_ENODEV. Any other ':', or any of "*<>?|, a control character or DEL, is
 * an invalid name, API_EINVAL. */
bool path_fat_ok(const char *path, bool after_drive, api_errno *err);

#endif /* _CORE_STR_PATH_H_ */
