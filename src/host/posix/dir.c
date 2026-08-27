/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Paths cross the seam spelled the way the 6502 spells them and in its OEM
 * code page. The drive prefix comes off with path_to_native() and the code
 * page with oem_to_utf8() (core/str/oem.h) before opendir; returned names go
 * back with oem_from_utf8().
 */

#include "host/api/dir.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define DIR_UPATH_MAX (3 * 4096) /* worst case: every OEM byte -> 3 UTF-8 bytes */

void *host_dir_open(const char *path)
{
    char native[HOST_MAX_PATH];
    char u8[DIR_UPATH_MAX];
    if (!path_to_native(path, native, sizeof native))
        return NULL;
    if (oem_to_utf8(native, u8, sizeof u8) >= sizeof u8)
    {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return opendir(u8);
}

int host_dir_read(void *d, char *name, size_t namesz, struct host_fs_meta *meta)
{
    errno = 0;
    struct dirent *de = readdir((DIR *)d);
    if (!de)
        return errno ? -1 : 0; /* errno set -> a real error, else end-of-directory */
    oem_from_utf8(de->d_name, name, namesz); /* truncation caps, like snprintf did */
    struct stat st;
    if (fstatat(dirfd((DIR *)d), de->d_name, &st, 0) == 0)
    {
        meta->is_dir = S_ISDIR(st.st_mode);
        meta->is_readonly = !(st.st_mode & S_IWUSR);
        meta->size = (uint64_t)st.st_size;
        meta->mtime = st.st_mtime;
        meta->crtime = st.st_ctime; /* POSIX has no birth time; report change time */
    }
    else
    {
        /* An entry that cannot be stat'd is still an entry. Report what the
         * directory itself said and nothing more, rather than failing a read
         * over one unreadable name. */
        memset(meta, 0, sizeof(*meta));
        meta->is_dir = (de->d_type == DT_DIR);
    }
    meta->is_hidden = (de->d_name[0] == '.'); /* POSIX convention: leading-dot names */
    return 1;
}

void host_dir_rewind(void *d)
{
    rewinddir((DIR *)d);
}

void host_dir_close(void *d)
{
    closedir((DIR *)d);
}
