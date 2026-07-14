/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/api/std.h"
#include "emu/host/msc.h"
#include "emu/plat.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

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

/* An open host file: a plain fd, flagged once it is written so the drive is persisted
 * when it closes. The read/write transfer is non-blocking in the fs seam. */
struct host_file
{
    bool used;
    int fd;
    bool wrote;
    bool writable; /* opened for write: lseek past EOF extends (else it clamps) */
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
     * file; the boot/exec loader reaches installs via install_resolve instead. */
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
 * path — getcwd is full-path-or-error). */
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
    int fd = fs_open(host, flags_to_posix(flags), 0666);
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
        fs_close(fd);
        *err = API_EMFILE;
        return -1;
    }
    files[des] = (struct host_file){.used = true, .fd = fd, .writable = (flags & 0x02) != 0};
    if (flags & 0x40) /* APPEND: one-time seek to EOF (O_TRUNC already ran) */
        fs_lseek(fd, 0, SEEK_END);
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
    bool wrote = f->wrote;
    int rc = 0;
    if (f->fd >= 0)
        rc = fs_close(f->fd);
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
    if (!f)
    {
        *got = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    std_rw_result r = fs_read(f->fd, buf, count, got);
    if (r == STD_ERROR)
        *err = msc_errno_to_api_errno(errno);
    return r;
}

std_rw_result msc_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err)
{
    struct host_file *f = host_fil(desc);
    if (!f)
    {
        *put = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    std_rw_result r = fs_write(f->fd, buf, count, put);
    if (r == STD_OK)
        f->wrote = true;
    else if (r == STD_ERROR)
        *err = msc_errno_to_api_errno(errno);
    return r;
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
    int64_t cur = fs_lseek(f->fd, 0, SEEK_CUR);
    if (cur < 0)
    {
        *err = msc_errno_to_api_errno(errno);
        return -1;
    }
    int64_t base;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = cur;
    else if (whence == SEEK_END)
    {
        base = fs_lseek(f->fd, 0, SEEK_END);
        fs_lseek(f->fd, cur, SEEK_SET);
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
    /* Match FatFs f_lseek (firmware fat_std_lseek): a read-only file clamps the
     * pointer to its size; a writable file is extended to the target. Plain POSIX
     * lseek would leave a read pointer past EOF and defer any extension to the
     * next write, diverging from hardware. */
    int64_t size = fs_lseek(f->fd, 0, SEEK_END);
    if (size < 0)
    {
        *err = msc_errno_to_api_errno(errno);
        return -1;
    }
    if (target > size)
    {
        if (!f->writable)
            target = size; /* read mode: clamp to EOF */
        else if (fs_ftruncate(f->fd, target) < 0) /* write mode: extend the file */
        {
            *err = msc_errno_to_api_errno(errno);
            return -1;
        }
    }
    int64_t np = fs_lseek(f->fd, target, SEEK_SET);
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
