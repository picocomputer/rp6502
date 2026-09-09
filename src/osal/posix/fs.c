/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Paths arrive in the 6502's OEM code page and may carry this drive's name;
 * path_to_utf8 in osal/posix/dir.h takes both off before every libc call.
 */

#include "osal/fs.h"
#include "osal/dir.h"
#include "osal/os.h"
#include "osal/posix/dir.h"
#include "osal/posix/errmap.h"
#include "core/str/oem.h"
#include <errno.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <utime.h>

/* A descriptor is this host's own fd, so there is no pool of open files here.
 * The OS cannot report the name a descriptor was opened by, and a savestate
 * needs that name to open the same file again, so this table keeps it.
 *
 * There are seventeen slots because core/api/std.c allows STD_FD_MAX of 16
 * open descriptors and the ROM image takes one more. Nothing frees a slot on
 * close, so seventeen is only enough because fs_keep reuses the slot holding
 * the same fd, and the OS hands a closed number straight back on the next
 * open. The table is searched rather than indexed, because an fd number is
 * the OS's and may be anything. The path is made absolute after the open
 * succeeds, so realpath resolves a file that was just created, and a later
 * chdir cannot move it. */
#define FS_KEPT_MAX 17
static struct
{
    int fd; /* the OS descriptor plus one, so 0 can mark a free slot */
    uint8_t flags;
    char path[API_PATH_MAX + 1];
} fs_kept[FS_KEPT_MAX];

static void fs_keep(int fd, const char *path, uint8_t flags)
{
    char *abs = os_dir_realpath(path);
    const char *keep = abs ? abs : path;
    int slot = -1;
    for (int i = 0; i < FS_KEPT_MAX; i++)
        if (fs_kept[i].fd == fd + 1)
        {
            slot = i;
            break;
        }
    for (int i = 0; slot < 0 && i < FS_KEPT_MAX; i++)
        if (!fs_kept[i].fd)
            slot = i;
    if (slot >= 0 && strlen(keep) <= API_PATH_MAX)
    {
        fs_kept[slot].fd = fd + 1;
        fs_kept[slot].flags = flags & (FS_RD | FS_WR);
        strcpy(fs_kept[slot].path, keep);
    }
    free(abs);
}

bool fs_std_ident(int desc, sst_cursor_t *c)
{
    for (int i = 0; i < FS_KEPT_MAX; i++)
        if (fs_kept[i].fd == desc + 1)
        {
            off_t at = lseek(desc, 0, SEEK_CUR);
            if (at < 0 || at > INT32_MAX)
                return false;
            sst_put_str(c, fs_kept[i].path, FS_PATH_SLOT);
            sst_put_u8(c, fs_kept[i].flags);
            sst_put_i32(c, (int32_t)at);
            return sst_ok(c);
        }
    return false;
}

int fs_std_reopen(sst_cursor_t *c, api_errno *err)
{
    char path[FS_PATH_SLOT];
    sst_get_str(c, path, FS_PATH_SLOT);
    uint8_t flags = sst_get_u8(c);
    int32_t pos = sst_get_i32(c);
    if (!sst_ok(c))
    {
        *err = API_EINVAL;
        return -1;
    }
    /* The access bits only. CREAT, EXCL, TRUNC and APPEND already happened
     * when the program opened the file, and repeating one would create or
     * empty the very file this is trying to find again. */
    int fd = fs_std_open(path, flags & (FS_RD | FS_WR), err);
    if (fd < 0)
        return -1;
    if (lseek(fd, pos, SEEK_SET) < 0)
    {
        *err = errno_to_api(errno);
        api_errno ignored;
        fs_std_close(fd, &ignored);
        return -1;
    }
    return fd;
}

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
    if ((flags & FS_TRUNC) && wr)
        o |= O_TRUNC;
    int fd = open(u8, o, 0666);
    if (fd < 0)
        *err = errno_to_api(errno); /* before free(), which may set errno itself */
    free(u8);
    /* POSIX lets O_RDONLY on a directory through, while Windows and FatFs both
     * refuse it. Refusing here matches them and is better than handing back a
     * descriptor whose every read fails. */
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode))
    {
        close(fd);
        *err = API_EACCES; /* FR_DENIED, as the other machines spell it */
        return -1;
    }
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
            /* Reporting success would hand back a descriptor at the start of a
             * file the program asked to append to. */
            *err = errno_to_api(errno);
            close(fd);
            return -1;
        }
    }
    fs_keep(fd, path, flags);
    return fd;
}

/* The loader has already resolved ":name" through its alias map, and this
 * host has no store of its own to create one in, so the write combination is
 * refused. */
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
    /* Moved to the top of the descriptor space, which open() reaches only
     * once everything below it is in use, so the ROM image stays out of the
     * low numbers a program's own files get. */
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
    fs_keep(fd, path, FS_RD); /* the image's own name, for the savestate */
    return fd;
}

bool fs_rom_remove(const char *name, api_errno *err)
{
    (void)name;
    *err = API_EACCES; /* an install here is a reference, so there is nothing to delete */
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
    /* The position comes back as a signed 32-bit value, 0xFFFFFFFF being the
     * error sentinel, so a target past 2GB-1 is refused before the pointer
     * moves rather than landing somewhere that cannot be reported. */
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
    /* Measured with fstat rather than a seek to the end, because a seek that
     * turns out to be impossible has to leave the pointer where it was. */
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
