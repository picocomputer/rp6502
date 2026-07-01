/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MSC0: the writable host filesystem driver: the catch-all in std.c's driver
 * table (it claims every path no earlier driver took, like the firmware's FatFs
 * driver). Open files are plain host fds in a small pool; paths are translated
 * through dir.c, which also owns the directory and metadata ops. A write marks the
 * file so closing it persists the drive (a no-op natively; web: Emscripten IDBFS).
 */

#include "emu/api/api.h" /* FS_HOST_MAX_PATH */
#include "emu/api/std.h"
#include "emu/host/dir.h" /* fs_to_host */
#include "emu/host/fs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#ifdef EMU_HAVE_AIO
#include <aio.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* Flush the MSC0: drive (Emscripten IDBFS) to IndexedDB so writes survive a
 * reload. Async/fire-and-forget. The tmpdrive lives in a RAM FatFs, so a sync
 * there persists nothing — it expires with the session. */
EM_JS(void, host_persist, (void), {
    if (typeof FS !== 'undefined')
        FS.syncfs(false, function (err) { if (err) console.error('syncfs(false)', err); });
});
#else
static void host_persist(void) {}
#endif

#define HOST_MAX_OPEN 16

/* Data transfers run as POSIX AIO under the windowed real-time loop so the 6502
 * keeps clocking while they complete (read_xram is the documented background DMA
 * into XRAM); synchronous otherwise — headless/tests stay deterministic and the
 * web build (no aio) uses the instant in-RAM MEMFS. */
static bool g_async;
void host_set_async(bool on) { g_async = on; }

/* An open host file: a plain fd, flagged once it is written so the drive is
 * persisted when it closes. Under g_async a single in-flight aiocb carries the
 * current read/write, polled to completion by the per-scanline RIA pump. */
struct host_file
{
    bool used;
    int fd;
    bool wrote;
#ifdef EMU_HAVE_AIO
    bool aio_active;
    struct aiocb cb;
#endif
};
static struct host_file files[HOST_MAX_OPEN];

static struct host_file *host_fil(int desc)
{
    if (desc < 0 || desc >= HOST_MAX_OPEN || !files[desc].used)
        return NULL;
    return &files[desc];
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
    if ((flags & 0x20) && wr) /* TRUNC, only when opened for write */
        o |= O_TRUNC;
    return o;
}

/* The fs backends report failures by setting POSIX errno; translate to the 6502 set. */
api_errno host_errno_to_api_errno(int host_errno)
{
    switch (host_errno)
    {
    case ENOENT:
        return API_ENOENT;
    case EACCES:
    case EPERM:
    case EROFS:
        return API_EACCES;
    case EEXIST:
        return API_EEXIST;
    case EINVAL:
    case EISDIR:
    case ENOTDIR:
    case ENOTEMPTY:
    case ENAMETOOLONG:
        return API_EINVAL;
    case ENOSPC:
    case EFBIG:
        return API_ENOSPC;
    case EMFILE:
    case ENFILE:
        return API_EMFILE;
    case EBADF:
        return API_EBADF;
    case ENODEV:
    case ENXIO:
        return API_ENODEV;
    case EAGAIN:
        return API_EAGAIN;
    case ENOMEM:
        return API_ENOMEM;
    case ESPIPE:
        return API_ESPIPE;
    case ERANGE:
        return API_ERANGE;
    default:
        return API_EIO;
    }
}

bool host_std_handles(const char *path)
{
    (void)path;
    return true; /* catch-all, registered last */
}

int host_std_open(const char *path, uint8_t flags, api_errno *err)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_to_host(path, host, sizeof(host)))
    {
        *err = host_errno_to_api_errno(errno);
        return -1;
    }
    int fd = open(host, flags_to_posix(flags), 0666);
    if (fd < 0)
    {
        *err = host_errno_to_api_errno(errno);
        return -1;
    }
    int des = 0;
    for (; des < HOST_MAX_OPEN; des++)
        if (!files[des].used)
            break;
    if (des == HOST_MAX_OPEN)
    {
        close(fd);
        *err = API_EMFILE;
        return -1;
    }
    files[des] = (struct host_file){.used = true, .fd = fd};
    if (flags & 0x40) /* APPEND: one-time seek to EOF (O_TRUNC already ran) */
        lseek(fd, 0, SEEK_END);
    return des;
}

std_rw_result host_std_close(int desc, api_errno *err)
{
    struct host_file *f = host_fil(desc);
    if (!f)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
#ifdef EMU_HAVE_AIO
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
    if (wrote)
        host_persist(); /* a saved file just closed: persist the drive (web: IDBFS) */
    return STD_OK;
}

std_rw_result host_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    struct host_file *f = host_fil(desc);
    *got = 0;
    if (!f)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
#ifdef EMU_HAVE_AIO
    if (g_async)
    {
        if (!f->aio_active)
        {
            off_t off = lseek(f->fd, 0, SEEK_CUR);
            if (off < 0)
            {
                *err = host_errno_to_api_errno(errno);
                return STD_ERROR;
            }
            memset(&f->cb, 0, sizeof(f->cb));
            f->cb.aio_fildes = f->fd;
            f->cb.aio_offset = off;
            f->cb.aio_buf = buf;
            f->cb.aio_nbytes = count;
            f->cb.aio_sigevent.sigev_notify = SIGEV_NONE;
            if (aio_read(&f->cb) != 0)
            {
                *err = host_errno_to_api_errno(errno);
                return STD_ERROR;
            }
            f->aio_active = true;
            return STD_PENDING;
        }
        int e = aio_error(&f->cb);
        if (e == EINPROGRESS)
            return STD_PENDING;
        f->aio_active = false;
        ssize_t r = aio_return(&f->cb);
        if (r < 0)
        {
            *err = host_errno_to_api_errno(e);
            return STD_ERROR;
        }
        if (r > 0)
            lseek(f->fd, r, SEEK_CUR); /* aio_read leaves the fd offset untouched */
        *got = (uint32_t)r;
        return STD_OK;
    }
#endif
    ssize_t r = read(f->fd, buf, count);
    if (r < 0)
    {
        *err = host_errno_to_api_errno(errno);
        return STD_ERROR;
    }
    *got = (uint32_t)r;
    return STD_OK;
}

std_rw_result host_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err)
{
    struct host_file *f = host_fil(desc);
    *put = 0;
    if (!f)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
#ifdef EMU_HAVE_AIO
    if (g_async)
    {
        if (!f->aio_active)
        {
            off_t off = lseek(f->fd, 0, SEEK_CUR);
            if (off < 0)
            {
                *err = host_errno_to_api_errno(errno);
                return STD_ERROR;
            }
            memset(&f->cb, 0, sizeof(f->cb));
            f->cb.aio_fildes = f->fd;
            f->cb.aio_offset = off;
            f->cb.aio_buf = (void *)buf;
            f->cb.aio_nbytes = count;
            f->cb.aio_sigevent.sigev_notify = SIGEV_NONE;
            if (aio_write(&f->cb) != 0)
            {
                *err = host_errno_to_api_errno(errno);
                return STD_ERROR;
            }
            f->aio_active = true;
            return STD_PENDING;
        }
        int e = aio_error(&f->cb);
        if (e == EINPROGRESS)
            return STD_PENDING;
        f->aio_active = false;
        ssize_t r = aio_return(&f->cb);
        if (r < 0)
        {
            *err = host_errno_to_api_errno(e);
            return STD_ERROR;
        }
        f->wrote = true;
        if (r > 0)
            lseek(f->fd, r, SEEK_CUR); /* aio_write leaves the fd offset untouched */
        *put = (uint32_t)r;
        return STD_OK;
    }
#endif
    ssize_t r = write(f->fd, buf, count);
    if (r < 0)
    {
        *err = host_errno_to_api_errno(errno);
        return STD_ERROR;
    }
    f->wrote = true;
    *put = (uint32_t)r;
    return STD_OK;
}

int host_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err)
{
    struct host_file *f = host_fil(desc);
    if (!f)
    {
        *err = API_EBADF;
        return -1;
    }
    /* The position is reported back as a signed 32-bit value (0xFFFFFFFF is the
     * error sentinel), so reject a target past 2GB-1 before moving the pointer,
     * leaving the file pointer where it was rather than at an unreportable spot. */
    off_t cur = lseek(f->fd, 0, SEEK_CUR);
    if (cur < 0)
    {
        *err = host_errno_to_api_errno(errno);
        return -1;
    }
    off_t base;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = cur;
    else if (whence == SEEK_END)
    {
        base = lseek(f->fd, 0, SEEK_END);
        lseek(f->fd, cur, SEEK_SET);
        if (base < 0)
        {
            *err = host_errno_to_api_errno(errno);
            return -1;
        }
    }
    else
    {
        *err = API_EINVAL;
        return -1;
    }
    off_t target = base + off;
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
    off_t np = lseek(f->fd, target, SEEK_SET);
    if (np < 0)
    {
        *err = host_errno_to_api_errno(errno);
        return -1;
    }
    *pos = (int32_t)np;
    return 0;
}

std_rw_result host_std_sync(int desc, api_errno *err)
{
    (void)desc, (void)err;
    host_persist();
    return STD_OK;
}
