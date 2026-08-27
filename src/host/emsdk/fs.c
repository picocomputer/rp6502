/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Emscripten filesystem primitives (core/api/fs.h). The same POSIX calls as host/posix/fs.c
 * over the instant in-RAM MEMFS, but the byte transfer is synchronous: fs_std_read/fs_std_write
 * complete in one call and never return STD_PENDING — a zero-latency read has nothing to
 * keep alive. Web is single-threaded with no POSIX aio, so it gets its own driver.
 *
 * Paths cross the seam in the guest's OEM code page. Convert to UTF-8 with
 * oem_to_utf8() (api/oem.h) before every libc call — the Emscripten runtime
 * decodes C paths as UTF-8 into MEMFS names — and returned paths back with
 * oem_from_utf8().
 */

#include "core/api/fs.h"
#include "host/api/fs.h"
#include "host/posix/errno.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include <emscripten.h>
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

/* MEMFS is RAM that goes away with the tab. The page mounts IDBFS over the
 * directory a program starts in and flushes it on pagehide; this is the same
 * flush from this side, so a save is durable when the guest closes the file
 * rather than only when the tab closes. Emscripten warns about overlapping
 * syncfs calls, so one is in flight at a time -- the write itself is async and
 * best-effort either way, which is why the page still flushes on the way out. */
EM_JS(void, web_idbfs_sync, (), {
    if (typeof FS === 'undefined' || globalThis.__rp6502_syncing)
        return;
    globalThis.__rp6502_syncing = true;
    try
    {
        FS.syncfs(false, function() { globalThis.__rp6502_syncing = false; });
    }
    catch (e)
    {
        globalThis.__rp6502_syncing = false;
    }
});

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
    /* A drive query names a drive, and no name is the one in use -- the same
     * rule opendir follows, and what f_getfree does with "". */
    if (!path[0])
        path = ".";
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

FILE *host_fs_fopen_rd(const char *path)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return NULL;
    return fopen(u8, "rb");
}

/* ---- The std driver ------------------------------------------------------ */

bool fs_std_handles(const char *path)
{
    (void)path;
    return true; /* catch-all, registered last */
}

static int fs_open_native(const char *path, uint8_t flags, api_errno *err)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
    {
        *err = errno_to_api(errno);
        return -1;
    }
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
        *err = errno_to_api(errno);
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
            *err = errno_to_api(errno);
            close(fd);
            return -1;
        }
    }
    return fd;
}

/* The ROM loader's own descriptor: read-only, one at a time, and moved out of
 * the range open(2) hands out so a program can neither name it nor be given
 * it. MEMFS descriptors start low and stay low, so anywhere well above them
 * is out of reach. */
#define ROM_FD 4000
static bool rom_open;

int fs_rom_open(const char *path, api_errno *err)
{
    if (rom_open)
    {
        close(ROM_FD);
        rom_open = false;
    }
    int fd = fs_open_native(path, FS_RD, err);
    if (fd < 0)
        return -1;
    if (fd != ROM_FD)
    {
        if (dup2(fd, ROM_FD) < 0)
        {
            *err = errno_to_api(errno);
            close(fd);
            return -1;
        }
        close(fd);
    }
    rom_open = true;
    return ROM_FD;
}

/* MEMFS is RAM that goes away with the tab, so a file the guest wrote has to
 * reach IDBFS before the descriptor does. Asking the descriptor whether it was
 * writable costs one call and saves the sync on every read-only close. */
std_rw_result fs_std_close(int desc, api_errno *err)
{
    int fl = fcntl(desc, F_GETFL);
    int rc = close(desc);
    if (desc == ROM_FD)
        rom_open = false;
    if (rc != 0)
    {
        *err = errno_to_api(errno);
        return STD_ERROR;
    }
    if (fl >= 0 && (fl & O_ACCMODE) != O_RDONLY)
        web_idbfs_sync();
    return STD_OK;
}

std_rw_result fs_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    ssize_t r = read(desc, buf, count);
    if (r < 0)
    {
        *got = 0;
        *err = errno_to_api(errno);
        return STD_ERROR;
    }
    *got = (uint32_t)r;
    return STD_OK;
}

std_rw_result fs_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err)
{
    ssize_t r = write(desc, buf, count);
    if (r < 0)
    {
        *put = 0;
        *err = errno_to_api(errno);
        return STD_ERROR;
    }
    *put = (uint32_t)r;
    return STD_OK;
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
    web_idbfs_sync();
    return STD_OK;
}
