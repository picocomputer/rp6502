/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * POSIX directory-stream access behind opaque handles, kept in its own TU so
 * host/dir.c can fill the FatFs FILINFO type without pulling <dirent.h> — whose
 * DIR typedef collides with FatFs ff.h's DIR.
 */

#ifndef _EMU_HOST_POSIXDIR_H_
#define _EMU_HOST_POSIXDIR_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

void *host_opendir_posix(const char *path); /* opaque stream, or NULL + errno */
/* 1 = an entry (name + is_dir filled), 0 = end of directory, -1 = error (errno). */
int host_readdir_posix(void *d, char *name, size_t namesz, bool *is_dir);
void host_rewinddir_posix(void *d);
void host_closedir_posix(void *d);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_POSIXDIR_H_ */
