/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The FatFs stdio file driver (extracted from usb/msc.c): open/close/read/write/
 * lseek/sync over a small FIL pool. FatFs-only — the block device (diskio) is
 * provided by the platform, so the desktop emulator reuses this over a RAM disk.
 */

#include "api/fat.h"
#include "fatfs/ff.h"
#include <assert.h>
#include <pico.h>
#include <stdio.h>

// File descriptor pool for open files
#define FAT_STD_FIL_MAX 8
static FIL fat_std_fil_pool[FAT_STD_FIL_MAX];

static FIL *fat_std_validate_fil(int desc)
{
    if (desc < 0 || desc >= FAT_STD_FIL_MAX)
        return NULL;
    if (!fat_std_fil_pool[desc].obj.fs)
        return NULL;
    return &fat_std_fil_pool[desc];
}

api_errno fat_fresult_to_api_errno(unsigned fresult)
{
    switch ((FRESULT)fresult)
    {
    case FR_DISK_ERR:
    case FR_INT_ERR:
    case FR_MKFS_ABORTED:
        return API_EIO;
    case FR_NOT_READY:
    case FR_INVALID_DRIVE:
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:
        return API_ENODEV;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return API_ENOENT;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
        return API_EINVAL;
    case FR_DENIED:
    case FR_WRITE_PROTECTED:
        return API_EACCES;
    case FR_EXIST:
        return API_EEXIST;
    case FR_INVALID_OBJECT:
        return API_EBADF;
    case FR_TIMEOUT:
        return API_EAGAIN;
    case FR_LOCKED:
        return API_EBUSY;
    case FR_NOT_ENOUGH_CORE:
        return API_ENOMEM;
    case FR_TOO_MANY_OPEN_FILES:
        return API_EMFILE;
    default:
        assert(false); // internal error
        return API_EIO;
    }
}

bool fat_std_handles(const char *path)
{
    (void)path;
    // MSC/FatFS is the catch-all handler
    return true;
}

int fat_std_open(const char *path, uint8_t flags, api_errno *err)
{
    // Low two bits of the public `flags` mirror FA_READ/FA_WRITE on purpose
    // so `flags & RDWR` passes straight through to f_open().
    static_assert(FA_READ == 0x01);
    static_assert(FA_WRITE == 0x02);
    const uint8_t RDWR = 0x03;
    const uint8_t CREAT = 0x10;
    const uint8_t TRUNC = 0x20;
    const uint8_t APPEND = 0x40;
    const uint8_t EXCL = 0x80;

    // Creation disposition picks the FatFs open mode; TRUNC/APPEND are applied
    // after open so they hold even without CREAT, per the POSIX open() contract.
    uint8_t mode = flags & RDWR;
    if (flags & CREAT)
        mode |= (flags & EXCL) ? FA_CREATE_NEW : FA_OPEN_ALWAYS;
    // else FA_OPEN_EXISTING (0): a missing file fails with FR_NO_FILE.

    FIL *fp = NULL;
    for (int i = 0; i < FAT_STD_FIL_MAX; i++)
    {
        if (!fat_std_fil_pool[i].obj.fs)
        {
            fp = &fat_std_fil_pool[i];
            break;
        }
    }
    if (!fp)
    {
        *err = API_EMFILE;
        return -1;
    }

    FRESULT fresult = f_open(fp, path, mode);
    if (fresult != FR_OK)
    {
        *err = fat_fresult_to_api_errno(fresult);
        return -1;
    }
    FRESULT post = FR_OK;
    if ((flags & TRUNC) && (mode & FA_WRITE))
        post = f_truncate(fp); // offset is 0 right after open
    if (post == FR_OK && (flags & APPEND))
        post = f_lseek(fp, f_size(fp));
    if (post != FR_OK)
    {
        f_close(fp);
        fp->obj.fs = NULL;
        *err = fat_fresult_to_api_errno(post);
        return -1;
    }

    return (int)(fp - fat_std_fil_pool);
}

std_rw_result fat_std_close(int desc, api_errno *err)
{
    FIL *fp = fat_std_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    FRESULT fresult = f_close(fp);
    fp->obj.fs = NULL;
    if (fresult != FR_OK)
    {
        *err = fat_fresult_to_api_errno(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}

std_rw_result fat_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    FIL *fp = fat_std_validate_fil(desc);
    if (!fp)
    {
        *bytes_read = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    UINT br;
    FRESULT fresult = f_read(fp, buf, count, &br);
    *bytes_read = br;
    if (fresult != FR_OK)
    {
        *err = fat_fresult_to_api_errno(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}

std_rw_result fat_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    FIL *fp = fat_std_validate_fil(desc);
    if (!fp)
    {
        *bytes_written = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    UINT bw;
    FRESULT fresult = f_write(fp, buf, count, &bw);
    *bytes_written = bw;
    if (fresult != FR_OK)
    {
        *err = fat_fresult_to_api_errno(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}

int fat_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err)
{
    FIL *fp = fat_std_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return -1;
    }
    FSIZE_t absolute_offset;
    if (whence == SEEK_SET)
    {
        if (offset < 0)
        {
            *err = API_EINVAL;
            return -1;
        }
        absolute_offset = offset;
    }
    else if (whence == SEEK_CUR)
    {
        FSIZE_t current_pos = f_tell(fp);
        if (offset < 0 && (FSIZE_t)(-offset) > current_pos)
        {
            *err = API_EINVAL;
            return -1;
        }
        if (offset > 0 && (FSIZE_t)offset > (~(FSIZE_t)0) - current_pos)
        {
            *err = API_EINVAL;
            return -1;
        }
        absolute_offset = current_pos + offset;
    }
    else if (whence == SEEK_END)
    {
        FSIZE_t file_size = f_size(fp);
        if (offset < 0 && (FSIZE_t)(-offset) > file_size)
        {
            *err = API_EINVAL;
            return -1;
        }
        if (offset > 0 && (FSIZE_t)offset > (~(FSIZE_t)0) - file_size)
        {
            *err = API_EINVAL;
            return -1;
        }
        absolute_offset = file_size + offset;
    }
    else
    {
        *err = API_EINVAL;
        return -1;
    }
    // *pos is returned as a signed 32-bit value (0xFFFFFFFF reserved for error),
    // so a target past 2GB-1 can't be represented. Reject it here, before the
    // seek, leaving the file pointer where it was rather than moving it to an
    // offset we could not report back.
    if (absolute_offset > 0x7FFFFFFF)
    {
        *err = API_ERANGE;
        return -1;
    }
    FRESULT fresult = f_lseek(fp, absolute_offset);
    if (fresult != FR_OK)
    {
        *err = fat_fresult_to_api_errno(fresult);
        return -1;
    }
    *pos = (int32_t)f_tell(fp);
    return 0;
}

std_rw_result fat_std_sync(int desc, api_errno *err)
{
    FIL *fp = fat_std_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    FRESULT fresult = f_sync(fp);
    if (fresult != FR_OK)
    {
        *err = fat_fresult_to_api_errno(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}
