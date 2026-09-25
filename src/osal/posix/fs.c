/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Paths arrive in the 6502's OEM code page and may carry this drive's name;
 * path_to_utf8 in osal/posix/dir.h takes both off before every libc call. A
 * SAVE: name and an installed ROM's host path are not drive paths and do not
 * go through it.
 */

#include "osal/fs.h"
#include "osal/os.h"
#include "osal/posix/dir.h"
#include "osal/posix/errmap.h"
#include "osal/posix/fs.h"
#include "core/str/str.h"
#include "host/host.h"
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
#ifdef __EMSCRIPTEN__
#include "osal/emscripten/os.h"
#endif

/* A descriptor is this host's own fd, so there is no pool of open files here.
 * The OS cannot report the name a descriptor was opened by, and a savestate
 * needs that name to open the same file again, so this table keeps it.
 *
 * A slot is freed when its descriptor closes, so sixteen cover every
 * descriptor core/api/std.c can hold. The table is searched rather than
 * indexed, because an fd number is the OS's and may be anything. */
#define FS_KEPT_MAX 16
static struct
{
    int fd; /* the OS descriptor plus one, so 0 can mark a free slot */
    uint8_t flags;
    char path[API_PATH_MAX + 1];
} fs_kept[FS_KEPT_MAX];

#ifdef __EMSCRIPTEN__
/* Whether the syncfs being dispatched has started its FS.syncfs. Only one call
 * is in flight at a time, so one flag serves every descriptor, and a close
 * clears it, so a sync that a stop interrupted never completes a later one. */
static bool fs_syncing;
#endif

/* A name that cannot be kept takes no slot, so an ident of the descriptor
 * fails rather than recording another file's name. */
static void fs_keep(int fd, const char *name, uint8_t flags)
{
    if (!name || strlen(name) > API_PATH_MAX)
        return;
    for (int i = 0; i < FS_KEPT_MAX; i++)
        if (!fs_kept[i].fd)
        {
            fs_kept[i].fd = fd + 1;
            fs_kept[i].flags = flags & (FS_RD | FS_WR);
            strcpy(fs_kept[i].path, name);
            return;
        }
}

void fs_closing(int desc)
{
    for (int i = 0; i < FS_KEPT_MAX; i++)
        if (fs_kept[i].fd == desc + 1)
            fs_kept[i].fd = 0;
#ifdef __EMSCRIPTEN__
    fs_syncing = false;
    /* IDBFS stores a file that was open for writing when it closes. */
    int fl = fcntl(desc, F_GETFL);
    if (fl >= 0 && (fl & O_ACCMODE) != O_RDONLY)
        os_estimate_stale();
#endif
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
    flags &= FS_RD | FS_WR;
    int fd = save_std_handles(path) ? save_std_open(path, flags, err)
                                    : fs_std_open(path, flags, err);
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

static int fs_open_native(const char *host, uint8_t flags, api_errno *err)
{
    bool rd = flags & FS_RD, wr = flags & FS_WR;
    int o = wr ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (flags & FS_CREAT)
        o |= O_CREAT;
    if ((flags & FS_CREAT) && (flags & FS_EXCL))
        o |= O_EXCL;
    if ((flags & FS_TRUNC) && wr)
        o |= O_TRUNC;
    int fd = open(host, o, 0666);
    if (fd < 0)
    {
        *err = errno_to_api(errno);
        return -1;
    }
    /* POSIX lets O_RDONLY on a directory through, while Windows and FatFs both
     * refuse it. Refusing here matches them and is better than handing back a
     * descriptor whose every read fails. */
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode))
    {
        close(fd);
        *err = API_EACCES;
        return -1;
    }
    return fd;
}

/* The open used by fs_std_open and fs_save_open. name is what a savestate
 * records for the descriptor, or NULL to record the drive path of the file at
 * host. That path is found after the open, so realpath resolves a file just
 * created, and a later chdir cannot change it. */
static int fs_open_kept(const char *host, uint8_t flags, const char *name, api_errno *err)
{
    int fd = fs_open_native(host, flags, err);
    if (fd < 0)
        return -1;
    if ((flags & FS_APPEND) && lseek(fd, 0, SEEK_END) < 0) /* once, after any TRUNC */
    {
        /* Reporting success would return a descriptor positioned at the start
         * of a file opened with O_APPEND. */
        *err = errno_to_api(errno);
        close(fd);
        return -1;
    }
    char *abs = NULL;
    if (!name)
    {
        char *r = realpath(host, NULL);
        abs = r ? path_from_host(r) : NULL;
        free(r);
    }
    fs_keep(fd, name ? name : abs, flags);
    free(abs);
    return fd;
}

int fs_std_open(const char *path, uint8_t flags, api_errno *err)
{
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return -1;
    int fd = fs_open_kept(u8, flags, NULL, err);
    free(u8);
    return fd;
}

static char *fs_save_dir;

void fs_save_start(void)
{
    const char *dir = host_save_dir();
    free(fs_save_dir);
    fs_save_dir = dir ? strdup(dir) : getcwd(NULL, 0);
}

void fs_save_free(void)
{
    free(fs_save_dir), fs_save_dir = NULL;
}

/* The name keeps to the ASCII that core/api/save.h allows, which is the same
 * in UTF-8 as in the code page. */
int fs_save_open(const char *name, uint8_t flags, api_errno *err)
{
    if (!fs_save_dir)
    {
        *err = API_ENODEV;
        return -1;
    }
    size_t sz = strlen(fs_save_dir) + 1 + strlen(name) + 1;
    char *host = malloc(sz);
    if (!host)
    {
        *err = API_ENOMEM;
        return -1;
    }
    snprintf(host, sz, "%s/%s", fs_save_dir, name);
    if (flags & FS_CREAT)
        os_ensure_parent_dir(host);
    char kept[STR_SAVE_COLON_LEN + SAVE_NAME_MAX + 1];
    snprintf(kept, sizeof kept, "%s%s", STR_SAVE_COLON, name);
    int fd = fs_open_kept(host, flags, kept, err);
    free(host);
    return fd;
}

char *fs_host_realpath(const char *host)
{
    return realpath(host, NULL);
}

/* Moved to the top of the descriptor space, which open() uses only once every
 * lower number is in use, so the ROM image does not take the low numbers that
 * a program's files get. Nothing is recorded for a savestate, because
 * core/rom/asset.c opens the image again by the name it was run by. */
int fs_rom_open_host(const char *host, api_errno *err)
{
    int fd = fs_open_native(host, FS_RD, err);
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

/* The loader has already resolved ":name" through its alias map, and this
 * host has no list of installed ROMs to add one to, so the write combination
 * is refused. */
int fs_rom_open(const char *path, uint8_t flags, api_errno *err)
{
    if (flags != FS_RD)
    {
        *err = (flags == (FS_WR | FS_CREAT | FS_EXCL)) ? API_EACCES : API_EINVAL;
        return -1;
    }
    char *u8 = path_to_utf8(path, err);
    if (!u8)
        return -1;
    int fd = fs_rom_open_host(u8, err);
    free(u8);
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

/* fs_extend grows a writable file from size to target with zeros and returns
 * 0 or an errno. An extending ftruncate leaves a hole on most host file
 * systems and succeeds on a full drive, so the gap is allocated here and a
 * full drive fails the seek. A failure cuts the file back to size. */
#ifdef __EMSCRIPTEN__
/* MEMFS holds every byte in memory, so ftruncate already allocates the gap.
 * It also goes through setattr, where the browser page marks the file to be
 * stored in IndexedDB, and posix_fallocate does not. */
static int fs_extend(int desc, int64_t size, int64_t target)
{
    (void)size;
    return ftruncate(desc, (off_t)target) != 0 ? errno : 0;
}
#else
static int fs_zero_fill(int desc, int64_t size, int64_t target)
{
    static const char zeros[4096];
    for (int64_t at = size; at < target;)
    {
        size_t n = target - at < (int64_t)sizeof zeros ? (size_t)(target - at)
                                                       : sizeof zeros;
        ssize_t w = pwrite(desc, zeros, n, (off_t)at);
        if (w < 0)
            return errno;
        at += w;
    }
    return 0;
}

static int fs_extend(int desc, int64_t size, int64_t target)
{
#ifdef __APPLE__
    int e = fs_zero_fill(desc, size, target); /* macOS has no posix_fallocate */
#else
    int e = posix_fallocate(desc, (off_t)size, (off_t)(target - size));
    if (e == EOPNOTSUPP || e == EINVAL) /* a file system without fallocate */
        e = fs_zero_fill(desc, size, target);
#endif
    if (e)
        (void)!ftruncate(desc, (off_t)size);
    return e;
}
#endif

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
        else
        {
            int e = fs_extend(desc, size, target);
            if (e)
            {
                *err = errno_to_api(e); /* the pointer has not moved */
                return -1;
            }
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

/* fsync does nothing in the browser, where FS.syncfs stores the files and
 * reports completion in a callback on a later turn of the page's event
 * loop. */
std_rw_result fs_std_sync(int desc, api_errno *err)
{
#ifdef __EMSCRIPTEN__
    (void)desc;
    if (!fs_syncing)
    {
        os_syncfs_start();
        fs_syncing = true;
    }
    std_rw_result r = os_syncfs_poll(err);
    if (r != STD_PENDING)
    {
        fs_syncing = false;
        os_estimate_stale();
    }
    return r;
#else
    if (fsync(desc) != 0)
    {
        *err = errno_to_api(errno);
        return STD_ERROR;
    }
    return STD_OK;
#endif
}
