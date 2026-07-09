/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/plat.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>

void *dir_open(const char *path)
{
    return opendir(path);
}

int dir_read(void *d, char *name, size_t namesz, bool *is_dir)
{
    errno = 0;
    struct dirent *de = readdir((DIR *)d);
    if (!de)
        return errno ? -1 : 0; /* errno set -> a real error, else end-of-directory */
    snprintf(name, namesz, "%s", de->d_name);
    *is_dir = (de->d_type == DT_DIR);
    return 1;
}

void dir_rewind(void *d)
{
    rewinddir((DIR *)d);
}

void dir_close(void *d)
{
    closedir((DIR *)d);
}
