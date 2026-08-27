/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

/* Reading a directory, as a host OS keeps them. The filesystem seam's other
 * half -- see host/api/fs.h for who implements it and who does not, and note
 * the prefix belongs to the seam rather than the file, the way
 * host/posix/dir.c defines host_dir_read.
 *
 * Paths arrive spelled the way the 6502 spells them, drive prefix and all,
 * exactly as they do at host/api/fs.h.
 *
 * Not to be confused with core/api/dir.h, which is the 6502's directory
 * syscalls. That layer asks this one for the entries it hands back.
 */

#ifndef _HOST_API_DIR_H_
#define _HOST_API_DIR_H_

#include "host/api/fs.h"
#include <stdbool.h>
#include <stddef.h>

void *host_dir_open(const char *path); /* opaque stream, or NULL + errno */

/* One entry: 1 = name and meta filled, 0 = end of directory, -1 = error
 * (errno). The metadata comes back with the name because the host already
 * has it -- a FAT directory carries it in the entry, and a POSIX one is a
 * single fstatat away against a descriptor already open. The alternative is
 * for the layer above to rebuild a full path per entry and stat it, which is
 * both slower and a spelling problem it should not have to solve. */
int host_dir_read(void *d, char *name, size_t namesz, struct host_fs_meta *meta);

void host_dir_rewind(void *d);
void host_dir_close(void *d);

#endif /* _HOST_API_DIR_H_ */
