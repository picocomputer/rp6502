/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * FatFs speaks FRESULT and its own code page; the API speaks api_errno and
 * OEM. This is the translation both halves of the filesystem share -- the
 * file driver in api/fs.c, the drive in api/dir.c, and the ROM loader's own
 * FIL in mon/rom.c.
 */

#include "api/fat.h"
#include "core/api/dir.h" /* oem_fs_code_page */
#include "fatfs/ff.h"
#include <assert.h>

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

/* FatFs converts filenames through its own active page, so it is told. */
void oem_fs_code_page(uint16_t cp)
{
    f_setcp(cp);
}
