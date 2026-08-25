/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

/* Reading a directory, as a host OS keeps them. The filesystem seam's other
 * half -- see host/fs.h for who implements it and who does not.
 */

#ifndef _CORE_DIR_H_
#define _CORE_DIR_H_

#include <stdbool.h>
#include <stddef.h>

void *dir_open(const char *path); /* opaque stream, or NULL + errno */
/* 1 = an entry (name + is_dir filled), 0 = end of directory, -1 = error (errno). */
int dir_read(void *d, char *name, size_t namesz, bool *is_dir);
void dir_rewind(void *d);
void dir_close(void *d);

#endif /* _CORE_DIR_H_ */
