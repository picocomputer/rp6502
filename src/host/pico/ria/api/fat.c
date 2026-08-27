/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * FatFs speaks FRESULT and its own code page. The filesystem seam speaks
 * POSIX errno and the API speaks api_errno, so a FRESULT becomes one and then
 * the other -- through a single table, so a failure means the same thing
 * whichever way the firmware asks. Shared by the file driver in api/fs.c, the
 * drive in api/dir.c, and the ROM loader's own FIL in mon/rom.c.
 */

#include "api/fat.h"
#include "core/api/dir.h" /* oem_fs_code_page */
#include "core/api/fs.h"  /* fs_errno_to_api_errno */
#include "fatfs/ff.h"
#include <assert.h>
#include <errno.h>

int fat_fresult_to_errno(unsigned fresult)
{
    switch ((FRESULT)fresult)
    {
    case FR_DISK_ERR:
    case FR_INT_ERR:
    case FR_MKFS_ABORTED:
        return EIO;
    case FR_NOT_READY:
    case FR_INVALID_DRIVE:
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:
        return ENODEV;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return ENOENT;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
        return EINVAL;
    case FR_DENIED:
    case FR_WRITE_PROTECTED:
        /* FR_DENIED is both a wrong access mode and a volume with no free
         * cluster left, and nothing distinguishes them -- which is why
         * API_ENOSPC never comes out of a FAT volume. */
        return EACCES;
    case FR_EXIST:
        return EEXIST;
    case FR_INVALID_OBJECT:
        return EBADF;
    case FR_TIMEOUT:
        return EAGAIN;
    case FR_LOCKED:
        return EBUSY;
    case FR_NOT_ENOUGH_CORE:
        return ENOMEM;
    case FR_TOO_MANY_OPEN_FILES:
        return EMFILE;
    default:
        assert(false); // internal error
        return EIO;
    }
}

void fat_fail(unsigned fresult)
{
    errno = fat_fresult_to_errno(fresult);
}

api_errno fat_fresult_to_api_errno(unsigned fresult)
{
    return fs_errno_to_api_errno(fat_fresult_to_errno(fresult));
}

/* FatFs converts filenames through its own active page, so it is told. */
void oem_fs_code_page(uint16_t cp)
{
    f_setcp(cp);
}
