/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Paths cross the seam in the guest's OEM code page. Convert to the host's
 * UTF-8 with oem_to_utf8() (core/str/oem.h) before every libc call, and
 * returned names/paths back with oem_from_utf8(). Fallible calls set errno,
 * which errmap.c turns into the api_errno the contract answers in.
 */

#include "osal/fs.h"
#include "osal/os.h"
#include "osal/posix/dir.h"
#include "osal/posix/errmap.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include <errno.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <utime.h>

/* ---- The std driver ------------------------------------------------------ */

/* A descriptor is this host's own fd. std.c hands back whatever open returned,
 * and the OS validates it on every call, so there is no pool here. */

bool fs_std_handles(const char *path)
{
    (void)path;
    return true; /* catch-all, registered last */
}

static int fs_open_native(const char *path, uint8_t flags, api_errno *err)
{
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return -1;
    bool rd = flags & FS_RD, wr = flags & FS_WR;
    int o = wr ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (flags & FS_CREAT)
        o |= O_CREAT;
    if ((flags & FS_CREAT) && (flags & FS_EXCL))
        o |= O_EXCL;
    if ((flags & FS_TRUNC) && wr) /* only when opened for write */
        o |= O_TRUNC;
    int fd = open(u8, o, 0666);
    if (fd < 0)
        *err = errno_to_api(errno); /* before the free, which may clobber it */
    free(u8);
    return fd;
}

int fs_std_open(const char *path, uint8_t flags, api_errno *err)
{
    int fd = fs_open_native(path, flags, err);
    if (fd < 0)
        return -1;
    if (flags & FS_APPEND) /* a one-time seek to the end, after any TRUNC */
    {
        if (lseek(fd, 0, SEEK_END) < 0)
        {
            /* Reporting success would hand back a descriptor positioned at the
             * start of a file the program asked to append to. */
            *err = errno_to_api(errno);
            close(fd);
            return -1;
        }
    }
    return fd;
}

/* The ROM descriptor: kept out of the range open(2) hands out so a program
 * can neither name it nor be given it. dup2 onto a descriptor above every
 * other -- the highest the process may hold -- is the cheapest way to say
 * that on POSIX. The loader resolves ":name" through its alias map before
 * this is called; nothing here spells a store, so the write combo has
 * nothing honest to create and refuses. */
int fs_rom_open(const char *path, uint8_t flags, api_errno *err)
{
    if (flags != FS_RD)
    {
        *err = (flags == (FS_WR | FS_CREAT | FS_EXCL)) ? API_EACCES : API_EINVAL;
        return -1;
    }
    int fd = fs_open_native(path, FS_RD, err);
    if (fd < 0)
        return -1;
    struct rlimit rl;
    int high = (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
                rl.rlim_cur > 1)
                   ? (int)rl.rlim_cur - 1
                   : -1;
    if (high > fd && dup2(fd, high) >= 0)
    {
        close(fd);
        fd = high;
    }
    return fd;
}

bool fs_rom_remove(const char *name, api_errno *err)
{
    (void)name;
    *err = API_EACCES; /* installs are references; there is nothing to delete */
    return false;
}

static int64_t fs_size_of(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;
    return (int64_t)st.st_size;
}

int fs_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err)
{
    int64_t base;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = (int64_t)lseek(desc, 0, SEEK_CUR);
    else if (whence == SEEK_END)
        base = fs_size_of(desc);
    else
    {
        *err = API_EINVAL;
        return -1;
    }
    if (base < 0)
    {
        *err = errno_to_api(errno);
        return -1;
    }
    /* The position comes back as a signed 32-bit value (0xFFFFFFFF is the
     * error sentinel), so a target past 2GB-1 is refused before the pointer
     * moves rather than landing somewhere unreportable. */
    int64_t target = base + off;
    if (target < 0)
    {
        *err = API_EINVAL;
        return -1;
    }
    if (target > 0x7FFFFFFF)
    {
        *err = API_ERANGE;
        return -1;
    }
    /* Measured with fstat, not a seek to the end: a seek that turns out to be
     * impossible must leave the pointer where it was. */
    int64_t size = fs_size_of(desc);
    if (size < 0)
    {
        *err = errno_to_api(errno);
        return -1;
    }
    if (target > size)
    {
        int fl = fcntl(desc, F_GETFL);
        if (fl < 0)
        {
            *err = errno_to_api(errno);
            return -1;
        }
        if ((fl & O_ACCMODE) == O_RDONLY)
            target = size; /* read-only: stop at the end */
        else if (ftruncate(desc, (off_t)target) != 0)
        {
            *err = errno_to_api(errno); /* no room: the pointer has not moved */
            return -1;
        }
    }
    int64_t np = lseek(desc, (off_t)target, SEEK_SET);
    if (np < 0)
    {
        *err = errno_to_api(errno);
        return -1;
    }
    *pos = (int32_t)np;
    return 0;
}

std_rw_result fs_std_sync(int desc, api_errno *err)
{
    if (fsync(desc) != 0)
    {
        *err = errno_to_api(errno);
        return STD_ERROR;
    }
    return STD_OK;
}
