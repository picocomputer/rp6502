/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See fs.h.
 */

#include "core/api/api.h"
#include "core/api/fs.h"
#include "core/api/std.h"
#include "host/api/fs.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h> /* SEEK_SET/CUR/END */
#include <string.h>

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

std_rw_result fs_io_to_std_result(host_io_result r)
{
    switch (r)
    {
    case HOST_IO_OK:
        return STD_OK;
    case HOST_IO_PENDING:
        return STD_PENDING;
    case HOST_IO_ERROR:
        break;
    }
    return STD_ERROR;
}

/* The fs backends report failures by setting POSIX errno; translate to the 6502 set. */
api_errno fs_errno_to_api_errno(int host_errno)
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

bool fs_std_handles(const char *path)
{
    (void)path;
    return true; /* catch-all, registered last */
}

int fs_std_open(const char *path, uint8_t flags, api_errno *err)
{
    /* A name of nothing is not a name -- FatFs answers FR_INVALID_NAME, so
     * the firmware does too. An empty path is still the working directory
     * to opendir, which is why this is here and not in the translation. */
    if (!path[0])
    {
        *err = API_ENOENT;
        return -1;
    }
    int fd = host_fs_open(path, flags);
    if (fd < 0)
    {
        *err = fs_errno_to_api_errno(errno);
        return -1;
    }
    int des = 0;
    for (; des < HOST_MAX_OPEN; des++)
        if (!files[des].used)
            break;
    if (des == HOST_MAX_OPEN)
    {
        host_fs_close(fd);
        *err = API_EMFILE;
        return -1;
    }
    files[des] = (struct host_file){.used = true, .fd = fd, .writable = (flags & HOST_FS_WR) != 0};
    if (flags & 0x40) /* APPEND: a one-time seek to the end, after any TRUNC */
    {
        int64_t end = host_fs_size(fd);
        if (end < 0 || host_fs_seek(fd, (uint64_t)end) < 0)
        {
            /* Reporting success here would hand back a descriptor positioned at
             * the start of a file the guest asked to append to. */
            *err = fs_errno_to_api_errno(errno);
            files[des].used = false;
            host_fs_close(fd);
            return -1;
        }
    }
    return des;
}

std_rw_result fs_std_close(int desc, api_errno *err)
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
        rc = host_fs_close(f->fd);
    f->used = false;
    if (wrote)
        host_fs_persist(); /* a saved file just closed: persist the drive (web: IDBFS) */
    if (rc != 0) /* deferred flush failure (ENOSPC/EIO on network/overlay FS) */
    {
        *err = fs_errno_to_api_errno(errno);
        return STD_ERROR;
    }
    return STD_OK;
}

std_rw_result fs_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    struct host_file *f = host_fil(desc);
    if (!f)
    {
        *got = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    std_rw_result r = fs_io_to_std_result(host_fs_read(f->fd, buf, count, got));
    if (r == STD_ERROR)
        *err = fs_errno_to_api_errno(errno);
    return r;
}

std_rw_result fs_std_write(int desc, const char *buf, uint32_t count, uint32_t *put, api_errno *err)
{
    struct host_file *f = host_fil(desc);
    if (!f)
    {
        *put = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    std_rw_result r = fs_io_to_std_result(host_fs_write(f->fd, buf, count, put));
    if (r == STD_OK)
        f->wrote = true;
    else if (r == STD_ERROR)
        *err = fs_errno_to_api_errno(errno);
    return r;
}

int fs_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err)
{
    struct host_file *f = host_fil(desc);
    if (!f)
    {
        *err = API_EBADF;
        return -1;
    }
    int64_t base;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = host_fs_tell(f->fd);
    else if (whence == SEEK_END)
        base = host_fs_size(f->fd);
    else
    {
        *err = API_EINVAL;
        return -1;
    }
    if (base < 0)
    {
        *err = fs_errno_to_api_errno(errno);
        return -1;
    }
    /* The position is reported back as a signed 32-bit value (0xFFFFFFFF is the
     * error sentinel), so reject a target past 2GB-1 before moving the pointer,
     * leaving the file pointer where it was rather than at an unreportable spot. */
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
    int64_t np = host_fs_seek(f->fd, (uint64_t)target);
    if (np < 0)
    {
        *err = fs_errno_to_api_errno(errno);
        return -1;
    }
    /* A writable file is extended to the target, so landing short of it means
     * the volume ran out -- FatFs clips silently and says FR_OK. A read-only
     * file stopping at its end is the contract, not a failure. */
    if (np != target && f->writable)
    {
        *err = API_ENOSPC;
        return -1;
    }
    *pos = (int32_t)np;
    return 0;
}

std_rw_result fs_std_sync(int desc, api_errno *err)
{
    struct host_file *f = host_fil(desc);
    if (!f)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    if (!host_fs_fsync(f->fd))
    {
        *err = fs_errno_to_api_errno(errno);
        return STD_ERROR;
    }
    return STD_OK;
}
