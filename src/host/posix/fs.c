/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Paths cross the seam in the guest's OEM code page. Convert to the host's
 * UTF-8 with oem_to_utf8() (api/oem.h) before every libc call, and returned
 * names/paths back with oem_from_utf8(). Fallible calls set errno and return
 * false so the fs_errno_to_api_errno funnel in core/api/fs.c works unchanged.
 */

#include "host/api/fs.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <utime.h>

#define FS_UPATH_MAX (3 * 4096) /* worst case: every OEM byte -> 3 UTF-8 bytes */

/* A path arrives spelled the way the 6502 spells it. This drive is one
 * directory of a real filesystem, so the drive prefix comes off here and
 * what is left is the native path -- and then the code page comes off too. */
static bool path_to_utf8(const char *path, char *u8 /* [FS_UPATH_MAX] */)
{
    char native[HOST_MAX_PATH];
    if (!path_to_native(path, native, sizeof native))
        return false;
    if (oem_to_utf8(native, u8, FS_UPATH_MAX) >= FS_UPATH_MAX)
    {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

/* And back: what the OS answered, spelled for the 6502. */
static bool path_from_utf8(const char *u8, char *out, size_t outsz)
{
    char native[HOST_MAX_PATH];
    if (oem_from_utf8(u8, native, sizeof native) >= sizeof native ||
        !path_from_native(native, out, outsz))
    {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

bool host_fs_stat(const char *path, struct host_fs_meta *out)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    struct stat st;
    if (stat(u8, &st) != 0)
        return false;
    const char *base = path_basename(path);
    out->is_dir = S_ISDIR(st.st_mode);
    out->is_readonly = !(st.st_mode & S_IWUSR);
    out->is_hidden = (base[0] == '.'); /* POSIX convention: leading-dot names */
    out->size = (uint64_t)st.st_size;
    out->mtime = st.st_mtime;
    out->crtime = st.st_ctime; /* POSIX has no birth time; report change time */
    return true;
}

bool host_fs_freespace(const char *path, uint64_t *total_bytes, uint64_t *avail_bytes)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    struct statvfs vfs;
    if (statvfs(u8, &vfs) != 0)
        return false;
    uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    *total_bytes = (uint64_t)vfs.f_blocks * unit;
    *avail_bytes = (uint64_t)vfs.f_bavail * unit;
    return true;
}

bool host_fs_set_readonly(const char *path, bool readonly)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    struct stat st;
    if (stat(u8, &st) != 0)
        return false;
    mode_t m = st.st_mode & 07777;
    if (readonly)
        m &= ~(mode_t)(S_IWUSR | S_IWGRP | S_IWOTH);
    else
        m |= S_IWUSR;
    return chmod(u8, m) == 0;
}

bool host_fs_set_mtime(const char *path, time_t mtime)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    struct utimbuf ub;
    ub.actime = ub.modtime = mtime;
    return utime(u8, &ub) == 0;
}

bool host_fs_mkdir(const char *path)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    return mkdir(u8, 0777) == 0;
}

bool host_fs_chdir(const char *path)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    return chdir(u8) == 0;
}

bool host_fs_getcwd(char *buf, size_t sz)
{
    char u8[FS_UPATH_MAX];
    if (!getcwd(u8, sizeof u8))
        return false;
    return path_from_utf8(u8, buf, sz);
}

bool host_fs_realpath(const char *path, char *out, size_t outsz)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    char *r = realpath(u8, NULL);
    if (!r)
        return false;
    bool ok = path_from_utf8(r, out, outsz);
    free(r);
    return ok;
}

bool host_fs_rename(const char *oldp, const char *newp)
{
    char u8old[FS_UPATH_MAX];
    char u8new[FS_UPATH_MAX];
    if (!path_to_utf8(oldp, u8old) || !path_to_utf8(newp, u8new))
        return false;
    return rename(u8old, u8new) == 0; /* POSIX rename replaces an existing target */
}

bool host_fs_remove(const char *path)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    return remove(u8) == 0; /* removes a file or an empty directory */
}

int host_fs_open(const char *path, uint8_t flags)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return -1;
    bool rd = flags & HOST_FS_RD, wr = flags & HOST_FS_WR;
    int o = wr ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (flags & HOST_FS_CREAT)
        o |= O_CREAT;
    if ((flags & HOST_FS_CREAT) && (flags & HOST_FS_EXCL))
        o |= O_EXCL;
    if ((flags & HOST_FS_TRUNC) && wr) /* only when opened for write */
        o |= O_TRUNC;
    return open(u8, o, 0666);
}

FILE *host_fs_fopen_rd(const char *path)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return NULL;
    return fopen(u8, "rb");
}

int64_t host_fs_size(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;
    return (int64_t)st.st_size;
}

int64_t host_fs_tell(int fd)
{
    return (int64_t)lseek(fd, 0, SEEK_CUR);
}

/* fstat rather than lseek(SEEK_END) to measure: a seek that turns out to be
 * impossible must leave the pointer where it was, and moving it to the end to
 * ask how long the file is would strand it there. */
int64_t host_fs_seek(int fd, uint64_t pos)
{
    int64_t size = host_fs_size(fd);
    if (size < 0)
        return -1;
    if ((int64_t)pos > size)
    {
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0)
            return -1;
        if ((fl & O_ACCMODE) == O_RDONLY)
            pos = (uint64_t)size; /* read-only: stop at the end */
        else if (ftruncate(fd, (off_t)pos) != 0)
            return -1; /* no room: the pointer has not moved */
    }
    return (int64_t)lseek(fd, (off_t)pos, SEEK_SET);
}

bool host_fs_fsync(int fd)
{
    if (fsync(fd) != 0)
        return false;
    host_fs_persist();
    return true;
}

void host_fs_persist(void) {} /* a real host filesystem is already durable */
