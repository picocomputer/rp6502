/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/plat.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#ifdef _WIN32
#include <windows.h> /* mingw has no sys/statvfs.h; GetDiskFreeSpaceExA covers fs_freespace */
#else
#include <sys/statvfs.h>
#endif

bool fs_stat(const char *path, struct fs_meta *out)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    out->is_dir = S_ISDIR(st.st_mode);
    out->is_readonly = !(st.st_mode & S_IWUSR);
    out->is_hidden = (base[0] == '.'); /* POSIX convention: leading-dot names */
    out->size = (uint64_t)st.st_size;
    out->mtime = st.st_mtime;
    out->crtime = st.st_ctime; /* POSIX has no birth time; report change time */
    return true;
}

bool fs_freespace(const char *path, uint64_t *total_bytes, uint64_t *avail_bytes)
{
#ifdef _WIN32
    ULARGE_INTEGER avail, total;
    if (!GetDiskFreeSpaceExA(path, &avail, &total, NULL))
        return false;
    *total_bytes = total.QuadPart;
    *avail_bytes = avail.QuadPart;
    return true;
#else
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0)
        return false;
    uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    *total_bytes = (uint64_t)vfs.f_blocks * unit;
    *avail_bytes = (uint64_t)vfs.f_bavail * unit;
    return true;
#endif
}

bool fs_set_readonly(const char *path, bool readonly)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    mode_t m = st.st_mode & 07777;
    if (readonly)
        m &= ~(mode_t)(S_IWUSR | S_IWGRP | S_IWOTH);
    else
        m |= S_IWUSR;
    return chmod(path, m) == 0;
}

bool fs_set_mtime(const char *path, time_t mtime)
{
    struct utimbuf ub;
    ub.actime = ub.modtime = mtime;
    return utime(path, &ub) == 0;
}

bool fs_mkdir(const char *path)
{
#ifdef _WIN32
    return mkdir(path) == 0; /* mingw's mkdir takes no mode */
#else
    return mkdir(path, 0777) == 0;
#endif
}

bool fs_chdir(const char *path)
{
    return chdir(path) == 0;
}

bool fs_getcwd(char *buf, size_t sz)
{
    if (!getcwd(buf, sz))
        return false;
#ifdef _WIN32
    for (char *p = buf; *p; p++) /* rest of the codebase is '/'-separated */
        if (*p == '\\')
            *p = '/';
#endif
    return true;
}

bool fs_rename(const char *oldp, const char *newp)
{
    return rename(oldp, newp) == 0; /* POSIX rename replaces an existing target */
}

bool fs_remove(const char *path)
{
    return remove(path) == 0; /* removes a file or an empty directory */
}

void fs_localtime(time_t t, struct tm *out)
{
    localtime_r(&t, out);
}

int fs_strcasecmp(const char *a, const char *b)
{
    return strcasecmp(a, b);
}

char *fs_realpath(const char *path, char *resolved)
{
#ifdef _WIN32
    /* mingw/MSVC have no realpath(); _fullpath() doesn't check existence like
     * POSIX realpath() does, so stat() first to match the semantics callers rely on. */
    struct stat st;
    if (stat(path, &st) != 0)
        return NULL;
    if (!_fullpath(resolved, path, PATH_MAX))
        return NULL;
    for (char *p = resolved; *p; p++) /* rest of the codebase is '/'-separated */
        if (*p == '\\')
            *p = '/';
    return resolved;
#else
    return realpath(path, resolved);
#endif
}
