/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_HOST_POSIXDIR_H_
#define _EMU_HOST_POSIXDIR_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

void *posixdir_open(const char *path); /* opaque stream, or NULL + errno */
/* 1 = an entry (name + is_dir filled), 0 = end of directory, -1 = error (errno). */
int posixdir_read(void *d, char *name, size_t namesz, bool *is_dir);
void posixdir_rewind(void *d);
void posixdir_close(void *d);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_POSIXDIR_H_ */
