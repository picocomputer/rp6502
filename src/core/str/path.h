/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Path syntax: what can be answered by looking at the characters. Nothing
 * here opens, resolves, or asks a filesystem anything -- a path that names
 * nothing gets the same answers as one that does.
 *
 * What a drive is called is not one of those questions. It belongs to
 * whichever drive would have to answer it: osal/posix/dir.c and
 * osal/windows/dir.c each speak their host's own namespace, and
 * host/pico/ria/sys/path.c resolves a CWD through FatFs.
 */

#ifndef _CORE_STR_PATH_H_
#define _CORE_STR_PATH_H_

/* True if c separates path components. FatFs accepts both, so both are. */
#define path_is_sep(c) ((c) == '/' || (c) == '\\')

/* The text after the last separator. A path that has none is all basename. */
const char *path_basename(const char *path);

#endif /* _CORE_STR_PATH_H_ */
