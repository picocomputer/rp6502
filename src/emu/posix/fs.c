/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/plat.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

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
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0)
        return false;
    uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    *total_bytes = (uint64_t)vfs.f_blocks * unit;
    *avail_bytes = (uint64_t)vfs.f_bavail * unit;
    return true;
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
    return mkdir(path, 0777) == 0;
}

bool fs_chdir(const char *path)
{
    return chdir(path) == 0;
}

bool fs_getcwd(char *buf, size_t sz)
{
    return getcwd(buf, sz) != NULL;
}

bool fs_realpath(const char *path, char *out, size_t outsz)
{
    char *r = realpath(path, NULL);
    if (!r)
        return false;
    if (strlen(r) >= outsz)
    {
        free(r);
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(out, r, strlen(r) + 1);
    free(r);
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

int fs_open(const char *path, int flags, int mode)
{
    return open(path, flags, mode);
}

int fs_close(int fd)
{
    return close(fd);
}

fs_ssize_t fs_read(int fd, void *buf, size_t n)
{
    return read(fd, buf, n);
}

fs_ssize_t fs_write(int fd, const void *buf, size_t n)
{
    return write(fd, buf, n);
}

int64_t fs_lseek(int fd, int64_t off, int whence)
{
    return (int64_t)lseek(fd, (off_t)off, whence);
}

int fs_ftruncate(int fd, int64_t length)
{
    return ftruncate(fd, (off_t)length);
}

fs_ssize_t fs_pread(int fd, void *buf, size_t n, int64_t off)
{
    return pread(fd, buf, n, (off_t)off);
}
