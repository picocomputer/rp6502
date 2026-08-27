/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Walking a POSIX directory, in POSIX's own terms.
 *
 * This is a file of its own for one reason: <dirent.h> and FatFs's ff.h both
 * define a type called DIR, and they are not the same type. The drive next
 * door needs FILINFO out of ff.h, so it cannot also have <dirent.h> -- and
 * one of the two has to give. Nothing here interprets anything; it hands back
 * a name and a struct stat, and dir.c turns those into what the 6502 asked
 * for.
 *
 * Named dirwalk and not dirent for the same reason errmap is not errno: a
 * host directory can be on the include path, and a header named for a C
 * library one is then found in its place.
 */

#ifndef _HOST_POSIX_DIRWALK_H_
#define _HOST_POSIX_DIRWALK_H_

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>

/* An open stream, or NULL and errno. The path is UTF-8, as libc wants it. */
void *posix_opendir(const char *u8path);

/* One entry: 1 = name and st filled, 0 = end of directory, -1 = error (errno).
 * The stat comes from a descriptor already open rather than a rebuilt path.
 * An entry that cannot be stat'd is still an entry, so st is synthesized from
 * what the directory itself said -- the type, and nothing else true. */
int posix_readdir(void *d, char *u8name, size_t namesz, struct stat *st);

void posix_rewinddir(void *d);
void posix_closedir(void *d);

#endif /* _HOST_POSIX_DIRWALK_H_ */
