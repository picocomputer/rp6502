/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OSAL_POSIX_DIR_H_
#define _OSAL_POSIX_DIR_H_

#include "core/api/api.h"

/* A drive path as a host path. The result is allocated to fit, because
 * oem_to_utf8 answers how much room it wants, and the caller frees it. A NULL
 * return has set *err. */
char *path_to_utf8(const char *path, api_errno *err);

/* An absolute host path as a drive path, FS: and all, allocated for the
 * caller to free. NULL when the code page cannot hold a character of it or
 * FAT refuses one, because the result would name a different file or none. */
char *path_from_host(const char *host);

#endif /* _OSAL_POSIX_DIR_H_ */
