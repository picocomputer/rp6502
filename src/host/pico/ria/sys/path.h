/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_PATH_H_
#define _RIA_SYS_PATH_H_

#include <stdbool.h>
#include <stddef.h>

const char *path_abs(const char *path);

/* On a FAT volume f_stat returns a long file name in the case it was given,
 * so path_lookup_basename reads the parent directory and matches the name
 * without regard to case to find the case that is stored. out needs
 * FF_LFN_BUF + 1 bytes to hold any long file name. */
bool path_lookup_basename(const char *path, char *out, size_t out_size);

/* path_correct_basename replaces the basename of path in place with the case
 * stored on disk. It returns false only when the corrected path would not fit
 * in path_size. When the lookup fails, it leaves the path unchanged and
 * returns true. */
bool path_correct_basename(char *path, size_t path_size);

/* Two resolved absolute paths name the same file when their drives are the
 * same logical drive, as 0: and MSC0: are, and the rest is equal without
 * regard to case, as FAT compares names. Installed ROMs, :NAME, compare by
 * name. */
bool path_eq(const char *a, const char *b);

#endif /* _RIA_SYS_PATH_H_ */
