/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_STR_PATH_H_
#define _CORE_STR_PATH_H_

/* True if c separates path components. These are the two FatFs accepts
 * (ff.c IsSeparator), and every caller is a FatFs path. */
#define path_is_sep(c) ((c) == '/' || (c) == '\\')

const char *path_basename(const char *path);

#endif /* _CORE_STR_PATH_H_ */
