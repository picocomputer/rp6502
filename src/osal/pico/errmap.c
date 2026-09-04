/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See errmap.h.
 */

#include "osal/pico/errmap.h"
#include "fatfs/ff.h"
#include <littlefs/lfs.h>
#include <assert.h>

api_errno fresult_to_api(unsigned fresult)
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

api_errno lfs_error_to_api(int lfs_err)
{
    switch (lfs_err)
    {
    case LFS_ERR_IO:
    case LFS_ERR_CORRUPT:
    case LFS_ERR_NOATTR:
        return API_EIO;
    case LFS_ERR_NOENT:
        return API_ENOENT;
    case LFS_ERR_EXIST:
        return API_EEXIST;
    case LFS_ERR_NOTDIR:
    case LFS_ERR_ISDIR:
    case LFS_ERR_NOTEMPTY:
    case LFS_ERR_INVAL:
    case LFS_ERR_NAMETOOLONG:
        return API_EINVAL;
    case LFS_ERR_BADF:
        return API_EBADF;
    case LFS_ERR_FBIG:
    case LFS_ERR_NOSPC:
        return API_ENOSPC;
    case LFS_ERR_NOMEM:
        return API_ENOMEM;
    default:
        return API_EIO;
    }
}
