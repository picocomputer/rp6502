/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The only TU that includes <dirent.h>: it isolates POSIX DIR so callers can use
 * the FatFs FILINFO type (whose ff.h also typedefs DIR) without a collision.
 */

#include "emu/host/posixdir.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>

void *host_opendir_posix(const char *path)
{
    return opendir(path);
}

int host_readdir_posix(void *d, char *name, size_t namesz, bool *is_dir)
{
    errno = 0;
    struct dirent *de = readdir((DIR *)d);
    if (!de)
        return errno ? -1 : 0; /* errno set -> a real error, else end-of-directory */
    snprintf(name, namesz, "%s", de->d_name);
    *is_dir = (de->d_type == DT_DIR);
    return 1;
}

void host_rewinddir_posix(void *d)
{
    rewinddir((DIR *)d);
}

void host_closedir_posix(void *d)
{
    closedir((DIR *)d);
}
