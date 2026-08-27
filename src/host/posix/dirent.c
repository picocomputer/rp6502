/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See dirent.h.
 */

#include "host/posix/dirent.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

void *posix_opendir(const char *u8path)
{
    return opendir(u8path);
}

int posix_readdir(void *d, char *u8name, size_t namesz, struct stat *st)
{
    DIR *dp = (DIR *)d;
    errno = 0;
    struct dirent *de = readdir(dp);
    if (!de)
        return errno ? -1 : 0; /* errno set -> a real error, else end-of-directory */
    snprintf(u8name, namesz, "%s", de->d_name);
    if (fstatat(dirfd(dp), de->d_name, st, 0) != 0)
    {
        memset(st, 0, sizeof(*st));
        st->st_mode = (de->d_type == DT_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    }
    return 1;
}

void posix_rewinddir(void *d)
{
    rewinddir((DIR *)d);
}

void posix_closedir(void *d)
{
    closedir((DIR *)d);
}
