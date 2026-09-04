/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The read/write/close slots of the POSIX file driver, done asynchronously: a
 * transfer is started, the dispatcher is told STD_PENDING, and the same call
 * reaps it on a later scanline. fs_sync.c answers the same three slots the
 * other way, and a machine names the one it wants.
 */

#include "osal/fs.h"
#include "osal/posix/errmap.h"
#include <aio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

/* The single in-flight transfer. fd < 0 means idle; only one exists at a time because
 * the guest syscall dispatcher is single-op (the same read/write is re-dispatched until
 * it completes). A reader from outside that dispatch -- the dropped-file screen, which
 * runs with a program still going -- is refused, not served this transfer. */
static struct
{
    struct aiocb cb;
    int fd;
} g_xfer = {.fd = -1};

static std_rw_result xfer_step(int fd, void *buf, uint32_t count, uint32_t *got, bool is_write)
{
    *got = 0;
    if (g_xfer.fd >= 0 && g_xfer.fd != fd)
    {
        /* The slot holds someone else's transfer. Reaping it here would hand
         * this caller that one's byte count and leave its buffer unwritten. */
        errno = EBUSY;
        return STD_ERROR;
    }
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

std_rw_result fs_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    std_rw_result r = xfer_step(desc, buf, count, got, false);
    if (r == STD_ERROR)
        *err = errno_to_api(errno);
    return r;
}

std_rw_result fs_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err)
{
    std_rw_result r = xfer_step(desc, (void *)buf, count, put, true);
    if (r == STD_ERROR)
        *err = errno_to_api(errno);
    return r;
}

std_rw_result fs_std_close(int desc, api_errno *err)
{
    int fd = desc;
    if (g_xfer.fd == fd) /* reap the in-flight transfer before the fd goes away */
    {
        const struct aiocb *cb = &g_xfer.cb;
        aio_cancel(fd, &g_xfer.cb);
        while (aio_error(&g_xfer.cb) == EINPROGRESS)
            aio_suspend(&cb, 1, NULL);
        aio_return(&g_xfer.cb);
        g_xfer.fd = -1;
    }
    if (close(fd) != 0) /* also a deferred flush failure: ENOSPC, EIO */
    {
        *err = errno_to_api(errno);
        return STD_ERROR;
    }
    return STD_OK;
}
