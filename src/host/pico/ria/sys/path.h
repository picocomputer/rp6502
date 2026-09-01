/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_PATH_H_
#define _RIA_SYS_PATH_H_

/* Path questions only the drive can answer, asked of FatFs. A machine whose
 * drive is a host filesystem resolves paths in its own dir.c instead, so
 * nothing here is shared -- the syntax both stand on is core/str/path.h. */

#include <stdbool.h>
#include <stddef.h>

/* Fully qualify a FatFs path, resolving the CWD for a relative one. Returns
 * static storage valid until the next call, or NULL when the path exceeds 255
 * characters or the CWD lookup fails. */
const char *path_abs(const char *path);

/* Look up the on-disk filename for path, case-insensitively. f_stat answers
 * with the case it was asked in, so the parent is enumerated to recover the
 * case really stored. out_size must hold any FatFs LFN (FF_LFN_BUF + 1). */
bool path_lookup_basename(const char *path, char *out, size_t out_size);

/* Replace path's basename in place with the case stored on disk. False only
 * when the corrected path would not fit, which the caller should treat as
 * fatal; a lookup that simply fails leaves the path alone and returns true. */
bool path_correct_basename(char *path, size_t path_size);

#endif /* _RIA_SYS_PATH_H_ */
