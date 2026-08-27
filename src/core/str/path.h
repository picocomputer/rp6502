/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Path syntax: what can be answered by looking at the characters. Nothing
 * here opens, resolves, or asks a filesystem anything -- a path that names
 * nothing gets the same answers as one that does.
 *
 * The questions that do need a filesystem belong to whichever drive would
 * have to answer them: core/api/fs.c maps into a host's namespace, and
 * host/pico/ria/sys/path.c resolves a CWD through FatFs.
 */

#ifndef _CORE_STR_PATH_H_
#define _CORE_STR_PATH_H_

#include <stdbool.h>

/* True if c separates path components. FatFs accepts both, so both are. */
#define path_is_sep(c) ((c) == '/' || (c) == '\\')

/* The text after the last separator. A path that has none is all basename. */
const char *path_basename(const char *path);

/* Past this machine's drive prefix, if the path carries one. There is one
 * writable drive, so "0:" and "MSC0:" (case-insensitive) are it; anything
 * else keeps its prefix and is a relative name, which the drive then rejects
 * on its own terms. */
const char *path_strip_drive(const char *path);

/* Whether path_strip_drive would strip anything. */
bool path_has_drive(const char *path);

#endif /* _CORE_STR_PATH_H_ */
