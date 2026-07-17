/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Paths cross the seam in the guest's OEM code page. Convert to the host's
 * UTF-8 with oem_to_utf8() (api/oem.h) before opendir, and convert returned
 * names back with oem_from_utf8().
 */

#include "emu/plat.h"
#include "api/oem.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>

#define DIR_UPATH_MAX (3 * 4096) /* worst case: every OEM byte -> 3 UTF-8 bytes */

void *dir_open(const char *path)
{
    char u8[DIR_UPATH_MAX];
    if (oem_to_utf8(path, u8, sizeof u8) >= sizeof u8)
    {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return opendir(u8);
}

int dir_read(void *d, char *name, size_t namesz, bool *is_dir)
{
    errno = 0;
    struct dirent *de = readdir((DIR *)d);
    if (!de)
        return errno ? -1 : 0; /* errno set -> a real error, else end-of-directory */
    oem_from_utf8(de->d_name, name, namesz); /* truncation caps, like snprintf did */
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
