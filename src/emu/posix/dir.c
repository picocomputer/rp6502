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
#ifdef _WIN32
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* mingw's dirent carries no d_type; wrap DIR* with its path so dir_read can
 * stat() each entry instead. */
struct win_dir
{
    DIR *dir;
    char *path;
};
#endif

void *dir_open(const char *path)
{
#ifdef _WIN32
    DIR *dir = opendir(path);
    if (!dir)
        return NULL;
    struct win_dir *wd = malloc(sizeof(*wd));
    if (!wd || !(wd->path = strdup(path)))
    {
        closedir(dir);
        free(wd);
        errno = ENOMEM;
        return NULL;
    }
    wd->dir = dir;
    return wd;
#else
    return opendir(path);
#endif
}

int dir_read(void *d, char *name, size_t namesz, bool *is_dir)
{
#ifdef _WIN32
    struct win_dir *wd = (struct win_dir *)d;
    errno = 0;
    struct dirent *de = readdir(wd->dir);
    if (!de)
        return errno ? -1 : 0;
    snprintf(name, namesz, "%s", de->d_name);
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", wd->path, de->d_name);
    struct stat st;
    *is_dir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
    return 1;
#else
    errno = 0;
    struct dirent *de = readdir((DIR *)d);
    if (!de)
        return errno ? -1 : 0; /* errno set -> a real error, else end-of-directory */
    snprintf(name, namesz, "%s", de->d_name);
    *is_dir = (de->d_type == DT_DIR);
    return 1;
#endif
}

void dir_rewind(void *d)
{
#ifdef _WIN32
    rewinddir(((struct win_dir *)d)->dir);
#else
    rewinddir((DIR *)d);
#endif
}

void dir_close(void *d)
{
#ifdef _WIN32
    struct win_dir *wd = (struct win_dir *)d;
    closedir(wd->dir);
    free(wd->path);
    free(wd);
#else
    closedir((DIR *)d);
#endif
}
