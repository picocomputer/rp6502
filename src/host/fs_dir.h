/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

/* Reading a directory, as a host OS keeps them. The filesystem seam's other
 * half -- see host/fs.h for who implements it and who does not, and note the
 * prefix belongs to the seam rather than the file, the way host/posix/fs_aio.c
 * defines fs_read.
 *
 * Not to be confused with core/api/dir.h, which is the 6502's directory
 * syscalls. That layer asks this one for the entries it hands back.
 */

#ifndef _HOST_FS_DIR_H_
#define _HOST_FS_DIR_H_

#include <stdbool.h>
#include <stddef.h>

void *fs_dir_open(const char *path); /* opaque stream, or NULL + errno */
/* 1 = an entry (name + is_dir filled), 0 = end of directory, -1 = error (errno). */
int fs_dir_read(void *d, char *name, size_t namesz, bool *is_dir);
void fs_dir_rewind(void *d);
void fs_dir_close(void *d);

#endif /* _HOST_FS_DIR_H_ */
