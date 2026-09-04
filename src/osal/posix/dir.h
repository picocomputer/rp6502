/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* A path as the 6502 spells it, made native, for the files beside this one.
 *
 * Here rather than beside the opens because resolving a path is what a
 * directory is for: the drive name this takes off is the one this drive
 * answers to, and the cwd a relative path is resolved against is
 * drive_getcwd's.
 */

#ifndef _OSAL_POSIX_DIR_H_
#define _OSAL_POSIX_DIR_H_

#include "core/api/api.h"

/* Allocated to fit -- oem_to_utf8 answers how much room it wants -- so it
 * neither caps nor guesses. The caller frees. NULL sets *err.
 *
 * There is no pair going the other way: what this host answers with is a
 * native path already, so only the code page has to change, and the two calls
 * that answer a path (drive_getcwd, os_dir_realpath) do that themselves. */
char *path_to_utf8(const char *path, api_errno *err);

#endif /* _OSAL_POSIX_DIR_H_ */
