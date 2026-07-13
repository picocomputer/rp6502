/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host helpers the filesystem/clock tests need where POSIX and Win32 diverge:
 * a throwaway working directory and an environment-variable set. The rest of
 * each test drives the portable emu/plat.h fs_* seam directly.
 */

#ifndef _EMU_TESTS_HOSTCOMPAT_H_
#define _EMU_TESTS_HOSTCOMPAT_H_

#include <stdbool.h>
#include <stddef.h>

/* Make a fresh empty temp directory. Writes its path (guest encoding,
 * '/'-separated — ready for fs_chdir) to out; returns false on failure. */
bool host_make_tmpdir(char *out, size_t outsz);

/* setenv(name, value, 1) in the platform's spelling. */
void host_setenv(const char *name, const char *value);

#endif /* _EMU_TESTS_HOSTCOMPAT_H_ */
