/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The MSC0: file driver: the writable host filesystem, the catch-all in std.c's
 * driver table (it claims every path no earlier driver took, like the firmware's
 * FatFs msc driver). Open files are plain host fds; paths are translated through
 * mscpath.c. Directory and metadata ops live in mscdir.c. A write marks the file
 * so closing it persists the drive (a no-op natively; web: Emscripten IDBFS).
 */

#include "emu/api/api.h"
#include "emu/msc/msc.h"
#include "emu/msc/mscpath.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <aio.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* Flush the MSC0: drive (Emscripten IDBFS) to IndexedDB so writes survive a
 * reload. Async/fire-and-forget. The tmpdrive lives in MEMFS /tmp, which IDBFS
 * never wraps, so a sync there persists nothing — it expires with the session. */
EM_JS(void, msc_persist, (void), {
    if (typeof FS !== 'undefined')
        FS.syncfs(false, function (err) { if (err) console.error('syncfs(false)', err); });
});
#else
static void msc_persist(void) {}
#endif

#define MSC_MAX_OPEN 16

/* Data transfers run as POSIX AIO under the windowed real-time loop so the 6502
 * keeps clocking while they complete (read_xram is the documented background DMA
 * into XRAM); synchronous otherwise — headless/tests stay deterministic and the
 * web build (no aio) uses the instant in-RAM MEMFS. */
static bool g_async;
void msc_set_async(bool on) { g_async = on; }

/* An open host file: a plain fd, flagged once it is written so the drive is
 * persisted when it closes. Under g_async a single in-flight aiocb carries the
 * current read/write, polled to completion by the per-scanline RIA pump. */
struct msc_file
{
    bool used;
    int fd;
    bool wrote;
    bool append; /* O_APPEND: an async write reseeks to EOF on completion */
#ifndef __EMSCRIPTEN__
    bool aio_active;
    struct aiocb cb;
#endif
};
static struct msc_file files[MSC_MAX_OPEN];

static struct msc_file *alloc_file(void)
{
    for (int i = 0; i < MSC_MAX_OPEN; i++)
        if (!files[i].used)
        {
            files[i] = (struct msc_file){.used = true};
            return &files[i];
        }
    return NULL;
}

/* flags are the rp6502 SDK open() bits (see ria/usb/msc.c). */
static int flags_to_posix(uint8_t flags)
{
    bool rd = flags & 0x01, wr = flags & 0x02;
    int o = wr ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (flags & 0x10) /* CREAT */
        o |= O_CREAT;
    if ((flags & 0x10) && (flags & 0x80)) /* CREAT + EXCL */
        o |= O_EXCL;
    if (flags & 0x20) /* TRUNC */
        o |= O_TRUNC;
    if (flags & 0x40) /* APPEND */
        o |= O_APPEND;
    return o;
}

bool msc_std_handles(const char *path)
{
    (void)path;
    return true; /* catch-all, registered last */
}

void *msc_std_open(const char *path, uint8_t flags)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
        return NULL;
    int fd = open(host, flags_to_posix(flags), 0666);
    if (fd < 0)
        return NULL; /* errno set by open() */
    struct msc_file *f = alloc_file();
    if (!f)
    {
        close(fd);
        errno = EMFILE;
        return NULL;
    }
    f->fd = fd;
    f->append = (flags & 0x40) != 0;
    return f;
}

void msc_std_close(void *desc)
{
    struct msc_file *f = desc;
    if (!f || !f->used)
        return;
#ifndef __EMSCRIPTEN__
    if (f->aio_active) /* reap an in-flight transfer (a reset can close mid-op) */
    {
        const struct aiocb *l = &f->cb;
        aio_cancel(f->fd, &f->cb);
        while (aio_error(&f->cb) == EINPROGRESS)
            aio_suspend(&l, 1, NULL);
        aio_return(&f->cb);
        f->aio_active = false;
    }
#endif
    bool wrote = f->wrote;
    if (f->fd >= 0)
        close(f->fd);
    f->used = false;
    f->fd = -1;
    if (wrote)
        msc_persist(); /* a saved file just closed: persist the drive (web: IDBFS) */
}

io_result msc_std_read(void *desc, void *buf, size_t n, size_t *got)
{
    struct msc_file *f = desc;
    *got = 0;
#ifndef __EMSCRIPTEN__
    if (g_async)
    {
        if (!f->aio_active)
        {
            off_t off = lseek(f->fd, 0, SEEK_CUR);
            if (off < 0)
                return IO_ERROR;
            memset(&f->cb, 0, sizeof(f->cb));
            f->cb.aio_fildes = f->fd;
            f->cb.aio_offset = off;
            f->cb.aio_buf = buf;
            f->cb.aio_nbytes = n;
            f->cb.aio_sigevent.sigev_notify = SIGEV_NONE;
            if (aio_read(&f->cb) != 0)
                return IO_ERROR; /* errno set by aio_read */
            f->aio_active = true;
            return IO_PENDING;
        }
        int e = aio_error(&f->cb);
        if (e == EINPROGRESS)
            return IO_PENDING;
        f->aio_active = false;
        ssize_t r = aio_return(&f->cb);
        if (r < 0)
        {
            errno = e;
            return IO_ERROR;
        }
        if (r > 0)
            lseek(f->fd, r, SEEK_CUR); /* aio_read leaves the fd offset untouched */
        *got = (size_t)r;
        return IO_OK;
    }
#endif
    ssize_t r = read(f->fd, buf, n);
    if (r < 0)
        return IO_ERROR;
    *got = (size_t)r;
    return IO_OK;
}

io_result msc_std_write(void *desc, const void *buf, size_t n, size_t *put)
{
    struct msc_file *f = desc;
    *put = 0;
#ifndef __EMSCRIPTEN__
    if (g_async)
    {
        if (!f->aio_active)
        {
            off_t off = lseek(f->fd, 0, SEEK_CUR);
            if (off < 0)
                return IO_ERROR;
            memset(&f->cb, 0, sizeof(f->cb));
            f->cb.aio_fildes = f->fd;
            f->cb.aio_offset = off;
            f->cb.aio_buf = (void *)buf;
            f->cb.aio_nbytes = n;
            f->cb.aio_sigevent.sigev_notify = SIGEV_NONE;
            if (aio_write(&f->cb) != 0)
                return IO_ERROR;
            f->aio_active = true;
            return IO_PENDING;
        }
        int e = aio_error(&f->cb);
        if (e == EINPROGRESS)
            return IO_PENDING;
        f->aio_active = false;
        ssize_t r = aio_return(&f->cb);
        if (r < 0)
        {
            errno = e;
            return IO_ERROR;
        }
        f->wrote = true;
        if (r > 0)
            lseek(f->fd, f->append ? 0 : r, f->append ? SEEK_END : SEEK_CUR);
        *put = (size_t)r;
        return IO_OK;
    }
#endif
    ssize_t r = write(f->fd, buf, n);
    if (r < 0)
        return IO_ERROR;
    f->wrote = true;
    *put = (size_t)r;
    return IO_OK;
}

long msc_std_lseek(void *desc, long off, int whence)
{
    struct msc_file *f = desc;
    return (long)lseek(f->fd, off, whence);
}

void msc_std_sync(void *desc)
{
    (void)desc;
    msc_persist();
}
