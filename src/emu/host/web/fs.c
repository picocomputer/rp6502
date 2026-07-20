/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Emscripten filesystem primitives (emu/host/host.h). The same POSIX calls as posix/fs.c
 * over the instant in-RAM MEMFS, but the byte transfer is synchronous: fs_read/fs_write
 * complete in one call and never return STD_PENDING — a zero-latency read has nothing to
 * keep alive. Web is single-threaded with no POSIX aio, so it gets its own seam.
 *
 * Paths cross the seam in the guest's OEM code page. Convert to UTF-8 with
 * oem_to_utf8() (api/oem.h) before every libc call — the Emscripten runtime
 * decodes C paths as UTF-8 into MEMFS names — and returned paths back with
 * oem_from_utf8().
 */

#include "emu/host/host.h"
#include "ria/api/oem.h"
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

#define FS_UPATH_MAX (3 * 4096) /* worst case: every OEM byte -> 3 UTF-8 bytes */

static bool path_to_utf8(const char *path, char *u8 /* [FS_UPATH_MAX] */)
{
    if (oem_to_utf8(path, u8, FS_UPATH_MAX) >= FS_UPATH_MAX)
    {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

bool fs_stat(const char *path, struct fs_meta *out)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    struct stat st;
    if (stat(u8, &st) != 0)
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

bool fs_set_readonly(const char *path, bool readonly)
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

bool fs_set_mtime(const char *path, time_t mtime)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    struct utimbuf ub;
    ub.actime = ub.modtime = mtime;
    return utime(u8, &ub) == 0;
}

bool fs_mkdir(const char *path)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    return mkdir(u8, 0777) == 0;
}

bool fs_chdir(const char *path)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    return chdir(u8) == 0;
}

bool fs_getcwd(char *buf, size_t sz)
{
    char u8[FS_UPATH_MAX];
    if (!getcwd(u8, sizeof u8))
        return false;
    if (oem_from_utf8(u8, buf, sz) >= sz)
    {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

bool fs_realpath(const char *path, char *out, size_t outsz)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    char *r = realpath(u8, NULL);
    if (!r)
        return false;
    size_t need = oem_from_utf8(r, out, outsz);
    free(r);
    if (need >= outsz)
    {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

bool fs_rename(const char *oldp, const char *newp)
{
    char u8old[FS_UPATH_MAX];
    char u8new[FS_UPATH_MAX];
    if (!path_to_utf8(oldp, u8old) || !path_to_utf8(newp, u8new))
        return false;
    return rename(u8old, u8new) == 0; /* POSIX rename replaces an existing target */
}

bool fs_remove(const char *path)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return false;
    return remove(u8) == 0; /* removes a file or an empty directory */
}

int fs_open(const char *path, int flags, int mode)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return -1;
    return open(u8, flags, mode);
}

FILE *fs_fopen_rd(const char *path)
{
    char u8[FS_UPATH_MAX];
    if (!path_to_utf8(path, u8))
        return NULL;
    return fopen(u8, "rb");
}

int fs_close(int fd)
{
    return close(fd);
}

std_rw_result fs_read(int fd, char *buf, uint32_t count, uint32_t *got)
{
    ssize_t r = read(fd, buf, count);
    if (r < 0)
    {
        *got = 0;
        return STD_ERROR;
    }
    *got = (uint32_t)r;
    return STD_OK;
}

std_rw_result fs_write(int fd, const char *buf, uint32_t count, uint32_t *put)
{
    ssize_t r = write(fd, buf, count);
    if (r < 0)
    {
        *put = 0;
        return STD_ERROR;
    }
    *put = (uint32_t)r;
    return STD_OK;
}

int64_t fs_lseek(int fd, int64_t off, int whence)
{
    return (int64_t)lseek(fd, (off_t)off, whence);
}

int fs_ftruncate(int fd, int64_t length)
{
    return ftruncate(fd, (off_t)length);
}

/* Flush the MSC0: drive (IDBFS) to IndexedDB so writes survive a reload.
 * Async/fire-and-forget. The --tmpdrive lives in a RAM FatFs, so a sync there
 * persists nothing — it expires with the session. EM_JS emits an imported symbol,
 * so wrap it in a plain fs_sync the cross-TU caller (msc.c) can link against. */
EM_JS(void, web_idbfs_sync, (void), {
    if (typeof FS !== 'undefined')
        FS.syncfs(false, function (err) { if (err) console.error('syncfs(false)', err); });
});

void fs_sync(void) { web_idbfs_sync(); }
