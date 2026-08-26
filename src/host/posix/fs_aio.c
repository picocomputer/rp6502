/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The read/write/close half of the POSIX fs seam, done asynchronously: a
 * transfer is started, the guest dispatcher is told PENDING, and the same
 * call reaps it on a later scanline. fs_sync.c answers the same three
 * functions the other way, and a host root names the one it wants.
 */

#include "host/fs.h"
#include <aio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

/* The single in-flight transfer. fd < 0 means idle; only one exists at a time because
 * the guest syscall dispatcher is single-op (the same read/write is re-dispatched until
 * it completes). */
static struct
{
    struct aiocb cb;
    int fd;
} g_xfer = {.fd = -1};

static fs_io_result xfer_step(int fd, void *buf, uint32_t count, uint32_t *got, bool is_write)
{
    *got = 0;
    if (g_xfer.fd < 0)
    {
        off_t off = lseek(fd, 0, SEEK_CUR); /* aio positions explicitly; snapshot here */
        if (off < 0)
            return FS_IO_ERROR;
        memset(&g_xfer.cb, 0, sizeof g_xfer.cb);
        g_xfer.cb.aio_fildes = fd;
        g_xfer.cb.aio_offset = off;
        g_xfer.cb.aio_buf = buf;
        g_xfer.cb.aio_nbytes = count;
        g_xfer.cb.aio_sigevent.sigev_notify = SIGEV_NONE;
        if ((is_write ? aio_write(&g_xfer.cb) : aio_read(&g_xfer.cb)) != 0)
            return FS_IO_ERROR;
        g_xfer.fd = fd;
        return FS_IO_PENDING;
    }
    int e = aio_error(&g_xfer.cb);
    if (e == EINPROGRESS)
        return FS_IO_PENDING;
    ssize_t r = aio_return(&g_xfer.cb);
    g_xfer.fd = -1;
    if (r < 0)
    {
        errno = e; /* the async failure, not aio_return's own errno write */
        return FS_IO_ERROR;
    }
    if (r > 0)
        lseek(fd, g_xfer.cb.aio_offset + r, SEEK_SET); /* aio left the offset; advance it */
    *got = (uint32_t)r;
    return FS_IO_OK;
}

fs_io_result fs_read(int fd, char *buf, uint32_t count, uint32_t *got)
{
    return xfer_step(fd, buf, count, got, false);
}

fs_io_result fs_write(int fd, const char *buf, uint32_t count, uint32_t *put)
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
