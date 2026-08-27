/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Files on the FAT volume, as host/api/fs.h asks for them: a FIL pool, and
 * FatFs underneath. The seam's signatures are f_lseek's and f_truncate's on
 * purpose, so the answers here are one call each.
 *
 * Only the file half is here. The metadata and namespace calls beside it in
 * host/api/fs.h serve core/api/drive.c, which a machine with FAT does not
 * compile -- api/dir.c answers those natively, in FatFs's own vocabulary.
 *
 * The block device underneath is usb/msc.c.
 */

#include "api/fat.h"
#include "host/api/fs.h"
#include "fatfs/ff.h"
#include <assert.h>
#include <errno.h>

#define FAT_FIL_MAX 8
static FIL fat_fil_pool[FAT_FIL_MAX];

static FIL *fat_fil(int fd)
{
    if (fd < 0 || fd >= FAT_FIL_MAX || !fat_fil_pool[fd].obj.fs)
    {
        errno = EBADF;
        return NULL;
    }
    return &fat_fil_pool[fd];
}

int host_fs_open(const char *path, uint8_t flags)
{
    // The seam's low two bits mirror FA_READ/FA_WRITE on purpose, so the
    // access mode passes straight through to f_open().
    static_assert(FA_READ == HOST_FS_RD);
    static_assert(FA_WRITE == HOST_FS_WR);

    // Creation disposition picks the FatFs open mode; TRUNC is applied after
    // open so it holds even without CREAT, per the POSIX open() contract.
    uint8_t mode = flags & (HOST_FS_RD | HOST_FS_WR);
    if (flags & HOST_FS_CREAT)
        mode |= (flags & HOST_FS_EXCL) ? FA_CREATE_NEW : FA_OPEN_ALWAYS;
    // else FA_OPEN_EXISTING (0): a missing file fails with FR_NO_FILE.

    int fd = 0;
    for (; fd < FAT_FIL_MAX; fd++)
        if (!fat_fil_pool[fd].obj.fs)
            break;
    if (fd == FAT_FIL_MAX)
    {
        errno = EMFILE;
        return -1;
    }
    FIL *fp = &fat_fil_pool[fd];

    FRESULT fresult = f_open(fp, path, mode);
    if (fresult != FR_OK)
    {
        fat_fail(fresult);
        return -1;
    }
    if ((flags & HOST_FS_TRUNC) && (mode & FA_WRITE))
    {
        fresult = f_truncate(fp); // the offset is 0 right after open
        if (fresult != FR_OK)
        {
            f_close(fp);
            fp->obj.fs = NULL;
            fat_fail(fresult);
            return -1;
        }
    }
    return fd;
}

int host_fs_close(int fd)
{
    FIL *fp = fat_fil(fd);
    if (!fp)
        return -1;
    FRESULT fresult = f_close(fp);
    fp->obj.fs = NULL;
    if (fresult != FR_OK)
    {
        fat_fail(fresult);
        return -1;
    }
    return 0;
}

host_io_result host_fs_read(int fd, char *buf, uint32_t count, uint32_t *got)
{
    *got = 0;
    FIL *fp = fat_fil(fd);
    if (!fp)
        return HOST_IO_ERROR;
    UINT br;
    FRESULT fresult = f_read(fp, buf, count, &br);
    *got = br;
    if (fresult != FR_OK)
    {
        fat_fail(fresult);
        return HOST_IO_ERROR;
    }
    return HOST_IO_OK;
}

host_io_result host_fs_write(int fd, const char *buf, uint32_t count, uint32_t *put)
{
    *put = 0;
    FIL *fp = fat_fil(fd);
    if (!fp)
        return HOST_IO_ERROR;
    UINT bw;
    FRESULT fresult = f_write(fp, buf, count, &bw);
    *put = bw;
    if (fresult != FR_OK)
    {
        fat_fail(fresult);
        return HOST_IO_ERROR;
    }
    return HOST_IO_OK;
}

int64_t host_fs_size(int fd)
{
    FIL *fp = fat_fil(fd);
    return fp ? (int64_t)f_size(fp) : -1;
}

int64_t host_fs_tell(int fd)
{
    FIL *fp = fat_fil(fd);
    return fp ? (int64_t)f_tell(fp) : -1;
}

int64_t host_fs_seek(int fd, uint64_t pos)
{
    FIL *fp = fat_fil(fd);
    if (!fp)
        return -1;
    FRESULT fresult = f_lseek(fp, (FSIZE_t)pos);
    if (fresult != FR_OK)
    {
        fat_fail(fresult);
        return -1;
    }
    /* f_lseek expands a writable file to the target and says FR_OK whether or
     * not it got there, so falling short is how a full volume reports itself.
     * A read-only file stopping at its end is the contract, not a failure. */
    FSIZE_t landed = f_tell(fp);
    if (landed != (FSIZE_t)pos && (fp->flag & FA_WRITE))
    {
        errno = ENOSPC;
        return -1;
    }
    return (int64_t)landed;
}

bool host_fs_fsync(int fd)
{
    FIL *fp = fat_fil(fd);
    if (!fp)
        return false;
    FRESULT fresult = f_sync(fp);
    if (fresult != FR_OK)
    {
        fat_fail(fresult);
        return false;
    }
    return true;
}
