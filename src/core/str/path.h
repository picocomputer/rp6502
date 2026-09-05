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

/* True if c separates path components. FatFs accepts both, and so does Win32,
 * so both are. Not every machine agrees -- a backslash is an ordinary byte in
 * a POSIX filename -- which is why the drive that has to open a path is the
 * one that decides what its separators are, and this is only what the two
 * that take both have in common. */
#define path_is_sep(c) ((c) == '/' || (c) == '\\')

/* The text after the last '/'. A path that has none is all basename.
 *
 * Only a slash, deliberately. This is what names a ROM and what a program is
 * told it is called, and on a POSIX host a backslash is a character in a name
 * rather than a separator -- splitting on one there would answer with half a
 * filename. A machine whose separators are both takes them off before it asks
 * (osal/windows/dir.c slashes every path it hands back). */
const char *path_basename(const char *path);

#endif /* _CORE_STR_PATH_H_ */
