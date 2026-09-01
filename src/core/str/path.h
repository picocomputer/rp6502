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
 * have to answer them: osal/posix/dir.c and osal/windows/dir.c map into their
 * host's namespace, and host/pico/ria/sys/path.c resolves a CWD through
 * FatFs.
 */

#ifndef _CORE_STR_PATH_H_
#define _CORE_STR_PATH_H_

#include <stdbool.h>
#include <stddef.h>

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

/* Between the two spellings of a path on a machine whose drive is a host
 * filesystem. "MSC0:/x" is the native "/x", "MSC0:x" is relative to the
 * process cwd, and "MSC0://C/x" names a Windows drive as "C:/x". A path with
 * no drive prefix is already native and crosses unchanged, which is what lets
 * a host path from a command line go straight through.
 *
 * A path crosses as written -- an empty one stays empty, and the OS says
 * what it thinks of that. Only the directory calls give "" a meaning (the
 * working directory), and they say so themselves.
 *
 * to_native sets errno and returns false if the result does not fit;
 * from_native returns the length written, or 0 -- never a short path, since
 * getcwd is full-path-or-error. */
bool path_to_native(const char *path, char *out, size_t outsz);
size_t path_from_native(const char *native, char *out, size_t outsz);

#endif /* _CORE_STR_PATH_H_ */
