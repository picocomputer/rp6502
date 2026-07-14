/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/plat.h"
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#if defined(__ANDROID__)
// Mock POSIX AIO for Android since Bionic libc does not support <aio.h>
#define EINPROGRESS 115
#ifndef SIGEV_NONE
#define SIGEV_NONE 0
#endif
struct aiocb {
    int aio_fildes;
    off_t aio_offset;
    void *aio_buf;
    size_t aio_nbytes;
    struct {
        int sigev_notify;
    } aio_sigevent;
    // Mock internal fields
    ssize_t result;
    int error_code;
};
#define AIO_CANCELED 0
#define AIO_NOTCANCELED 1
#define AIO_ALLDONE 2

static inline int aio_read(struct aiocb *cb)
{
    cb->result = pread(cb->aio_fildes, cb->aio_buf, cb->aio_nbytes, cb->aio_offset);
    if (cb->result < 0) {
        cb->error_code = errno;
    } else {
        cb->error_code = 0;
    }
    return 0;
}

static inline int aio_write(struct aiocb *cb)
{
    cb->result = pwrite(cb->aio_fildes, cb->aio_buf, cb->aio_nbytes, cb->aio_offset);
    if (cb->result < 0) {
        cb->error_code = errno;
    } else {
        cb->error_code = 0;
    }
    return 0;
}

static inline int aio_error(const struct aiocb *cb)
{
    return cb->error_code;
}

static inline ssize_t aio_return(struct aiocb *cb)
{
    return cb->result;
}

static inline int aio_cancel(int fd, struct aiocb *cb)
{
    (void)fd; (void)cb;
    return AIO_ALLDONE;
}

static inline int aio_suspend(const struct aiocb *const list[], int n, const struct timespec *timeout)
{
    (void)list; (void)n; (void)timeout;
    return 0;
}
#else
#include <aio.h>
#endif
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

/* The single in-flight transfer. fd < 0 means idle; only one exists at a time because
 * the guest syscall dispatcher is single-op (the same read/write is re-dispatched until
 * it completes). */
static struct
{
    struct aiocb cb;
    int fd;
} g_xfer = {.fd = -1};

static std_rw_result xfer_step(int fd, void *buf, uint32_t count, uint32_t *got, bool is_write)
{
    *got = 0;
    if (g_xfer.fd < 0)
    {
        off_t off = lseek(fd, 0, SEEK_CUR); /* aio positions explicitly; snapshot here */
        if (off < 0)
            return STD_ERROR;
        memset(&g_xfer.cb, 0, sizeof g_xfer.cb);
        g_xfer.cb.aio_fildes = fd;
        g_xfer.cb.aio_offset = off;
        g_xfer.cb.aio_buf = buf;
        g_xfer.cb.aio_nbytes = count;
        g_xfer.cb.aio_sigevent.sigev_notify = SIGEV_NONE;
        if ((is_write ? aio_write(&g_xfer.cb) : aio_read(&g_xfer.cb)) != 0)
            return STD_ERROR;
        g_xfer.fd = fd;
        return STD_PENDING;
    }
    int e = aio_error(&g_xfer.cb);
    if (e == EINPROGRESS)
        return STD_PENDING;
    ssize_t r = aio_return(&g_xfer.cb);
    g_xfer.fd = -1;
    if (r < 0)
    {
        errno = e; /* the async failure, not aio_return's own errno write */
        return STD_ERROR;
    }
    if (r > 0)
        lseek(fd, g_xfer.cb.aio_offset + r, SEEK_SET); /* aio left the offset; advance it */
    *got = (uint32_t)r;
    return STD_OK;
}

std_rw_result fs_read(int fd, char *buf, uint32_t count, uint32_t *got)
{
    return xfer_step(fd, buf, count, got, false);
}

std_rw_result fs_write(int fd, const char *buf, uint32_t count, uint32_t *put)
{
    return xfer_step(fd, (void *)buf, count, put, true);
}

int fs_close(int fd)
{
    if (g_xfer.fd == fd) /* reap the in-flight transfer before the fd goes away */
    {
        const struct aiocb *cb = &g_xfer.cb;
        aio_cancel(fd, &g_xfer.cb);
        while (aio_error(&g_xfer.cb) == EINPROGRESS)
            aio_suspend(&cb, 1, NULL);
        aio_return(&g_xfer.cb);
        g_xfer.fd = -1;
    }
    return close(fd);
}

int64_t fs_lseek(int fd, int64_t off, int whence)
{
    return (int64_t)lseek(fd, (off_t)off, whence);
}

int fs_ftruncate(int fd, int64_t length)
{
    return ftruncate(fd, (off_t)length);
}
