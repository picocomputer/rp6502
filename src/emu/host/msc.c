/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/api/std.h"
#include "emu/host/msc.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
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
void msc_set_async(bool on) { g_async = on; }

/* An open host file: a plain fd, flagged once it is written so the drive is
 * persisted when it closes. Under g_async a single in-flight aiocb carries the
 * current read/write, polled to completion by the per-scanline RIA pump. */
struct host_file
{
    bool used;
    int fd;
    bool wrote;
    bool writable; /* opened for write: lseek past EOF extends (else it clamps) */
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

/* ---- Address translation: MSC0: <-> host path ---------------------------- */

/* Drop a recognized writable-drive prefix. FatFs recognizes only "0:".."9:" and
 * "MSC0:".."MSC9:" (case-insensitive); anything else keeps its prefix and is
 * treated as a relative name (the OS, not us, then rejects a bogus ":"). */
const char *msc_strip_drive(const char *path)
{
    const char *colon = strchr(path, ':');
    if (!colon || colon == path)
        return path;
    size_t n = (size_t)(colon - path);
    bool is_drive = (n == 1 && isdigit((unsigned char)path[0])) ||
                    (n == 4 && strncasecmp(path, "MSC", 3) == 0 &&
                     isdigit((unsigned char)path[3]));
    return is_drive ? colon + 1 : path;
}

bool msc_has_drive_prefix(const char *path)
{
    return msc_strip_drive(path) != path;
}

/* Map a drive-stripped MSC0: path to a host path. "//C/..." names a Windows
 * drive; everything else is the native path verbatim — absolute "/x" from the OS
 * root, relative "x" from the process cwd. The OS resolves "." and "..". */
static bool rest_to_host(const char *rest, char *host, size_t hsz)
{
    int w;
    if (rest[0] == '/' && rest[1] == '/' &&
        isalpha((unsigned char)rest[2]) && rest[3] == '/')
        w = snprintf(host, hsz, "%c:/%s", rest[2], rest + 4);
    else
        w = snprintf(host, hsz, "%s", rest[0] ? rest : ".");
    if (w < 0 || (size_t)w >= hsz)
    {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

bool msc_to_host(const char *path, char *host, size_t hsz)
{
    const char *rest = msc_strip_drive(path);
    /* A leading ":" is the null drive (installed ROMs, install.c) — never a host
     * path. Refuse it here so neither ":name" nor "MSC0::name" can map onto a host
     * file; the boot/exec loader reaches installs via fs_resolve_rom instead. */
    if (rest[0] == ':')
    {
        errno = ENOENT;
        return false;
    }
    return rest_to_host(rest, host, hsz);
}

/* Render a host path as an MSC0: path (the inverse for absolutes): a Windows
 * "C:/x" -> "MSC0://C/x", else the path tacked under MSC0:. Returns its length,
 * or 0 if it did not fit (the caller must treat 0 as a failure, never a short
 * path — getcwd is full-path-or-error). Used for argv[0] and getcwd. */
size_t msc_from_host(const char *hostpath, char *out, size_t outsz)
{
    int w;
    if (isalpha((unsigned char)hostpath[0]) && hostpath[1] == ':')
        w = snprintf(out, outsz, "MSC0://%c%s", hostpath[0], hostpath + 2);
    else
        w = snprintf(out, outsz, "MSC0:%s", hostpath);
    if (w < 0 || (size_t)w >= outsz)
        return 0;
    return (size_t)w;
}

/* The fs backends report failures by setting POSIX errno; translate to the 6502 set. */
api_errno msc_errno_to_api_errno(int host_errno)
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

bool msc_std_handles(const char *path)
{
    (void)path;
    return true; /* catch-all, registered last */
}

int msc_std_open(const char *path, uint8_t flags, api_errno *err)
{
    char host[MSC_MAX_PATH];
    if (!msc_to_host(path, host, sizeof(host)))
    {
        *err = msc_errno_to_api_errno(errno);
        return -1;
    }
    int fd = open(host, flags_to_posix(flags), 0666);
    if (fd < 0)
    {
        *err = msc_errno_to_api_errno(errno);
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
    files[des] = (struct host_file){.used = true, .fd = fd, .writable = (flags & 0x02) != 0};
    if (flags & 0x40) /* APPEND: one-time seek to EOF (O_TRUNC already ran) */
        lseek(fd, 0, SEEK_END);
    return des;
}

std_rw_result msc_std_close(int desc, api_errno *err)
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
    int rc = 0;
    if (f->fd >= 0)
        rc = close(f->fd);
    f->used = false;
    if (wrote)
        host_persist(); /* a saved file just closed: persist the drive (web: IDBFS) */
    if (rc != 0) /* deferred flush failure (ENOSPC/EIO on network/overlay FS) */
    {
        *err = msc_errno_to_api_errno(errno);
        return STD_ERROR;
    }
    return STD_OK;
}

std_rw_result msc_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
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
                *err = msc_errno_to_api_errno(errno);
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
                *err = msc_errno_to_api_errno(errno);
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
            *err = msc_errno_to_api_errno(e);
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
        *err = msc_errno_to_api_errno(errno);
        return STD_ERROR;
    }
    *got = (uint32_t)r;
    return STD_OK;
}

std_rw_result msc_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err)
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
                *err = msc_errno_to_api_errno(errno);
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
                *err = msc_errno_to_api_errno(errno);
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
            *err = msc_errno_to_api_errno(e);
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
        *err = msc_errno_to_api_errno(errno);
        return STD_ERROR;
    }
    f->wrote = true;
    *put = (uint32_t)r;
    return STD_OK;
}

int msc_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err)
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
        *err = msc_errno_to_api_errno(errno);
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
            *err = msc_errno_to_api_errno(errno);
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
    /* Match FatFs f_lseek (firmware fat_std_lseek): a read-only file clamps the
     * pointer to its size; a writable file is extended to the target. Plain POSIX
     * lseek would leave a read pointer past EOF and defer any extension to the
     * next write, diverging from hardware. */
    off_t size = lseek(f->fd, 0, SEEK_END);
    if (size < 0)
    {
        *err = msc_errno_to_api_errno(errno);
        return -1;
    }
    if (target > size)
    {
        if (!f->writable)
            target = size; /* read mode: clamp to EOF */
        else if (ftruncate(f->fd, target) < 0) /* write mode: extend the file */
        {
            *err = msc_errno_to_api_errno(errno);
            return -1;
        }
    }
    off_t np = lseek(f->fd, target, SEEK_SET);
    if (np < 0)
    {
        *err = msc_errno_to_api_errno(errno);
        return -1;
    }
    *pos = (int32_t)np;
    return 0;
}

std_rw_result msc_std_sync(int desc, api_errno *err)
{
    (void)desc, (void)err;
    host_persist();
    return STD_OK;
}
