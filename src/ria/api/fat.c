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
        *err = api_errno_from_fatfs(fresult);
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
        *err = api_errno_from_fatfs(post);
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
        *err = api_errno_from_fatfs(fresult);
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
        *err = api_errno_from_fatfs(fresult);
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
        *err = api_errno_from_fatfs(fresult);
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
        *err = api_errno_from_fatfs(fresult);
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
        *err = api_errno_from_fatfs(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}

// ---- Directory / metadata ops over FatFs (open dir pool + f_* wrappers) ----

static_assert(FF_USE_CHMOD == 1);
static_assert(FF_USE_LABEL == 1);
static_assert(FF_FS_CRTIME == 1);
static_assert(FF_FS_RPATH == 2);

#define FAT_DIR_MAX 8
static DIR fat_dirs[FAT_DIR_MAX];
static int32_t fat_tells[FAT_DIR_MAX]; // entry index, for telldir/seekdir

void fat_dir_run(void)
{
    for (int i = 0; i < FAT_DIR_MAX; i++)
        fat_dirs[i].obj.fs = 0;
}

void fat_dir_stop(void)
{
    for (int i = 0; i < FAT_DIR_MAX; i++)
    {
        f_closedir(&fat_dirs[i]);
        fat_dirs[i].obj.fs = 0;
    }
}

// Validate an open-directory descriptor (EINVAL out of range, EBADF unopened).
static bool fat_dir_valid(int des, api_errno *err)
{
    if (des < 0 || des >= FAT_DIR_MAX)
    {
        *err = API_EINVAL;
        return false;
    }
    if (fat_dirs[des].obj.fs == 0)
    {
        *err = API_EBADF;
        return false;
    }
    return true;
}

int fat_stat(const char *path, FILINFO *fno, api_errno *err)
{
    FRESULT fresult = f_stat(path, fno);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

int fat_opendir(const char *path, api_errno *err)
{
    int des = 0;
    for (; des < FAT_DIR_MAX; des++)
        if (fat_dirs[des].obj.fs == 0)
            break;
    if (des == FAT_DIR_MAX)
    {
        *err = API_EMFILE;
        return -1;
    }
    FRESULT fresult = f_opendir(&fat_dirs[des], path);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    fat_tells[des] = 0;
    return des;
}

int fat_readdir(int des, FILINFO *fno, api_errno *err)
{
    if (!fat_dir_valid(des, err))
        return -1;
    FRESULT fresult = f_readdir(&fat_dirs[des], fno);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    if (fno->fname[0])
        fat_tells[des]++;
    return 0;
}

int fat_closedir(int des, api_errno *err)
{
    if (!fat_dir_valid(des, err))
        return -1;
    FRESULT fresult = f_closedir(&fat_dirs[des]);
    fat_dirs[des].obj.fs = 0;
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

int fat_telldir(int des, int32_t *pos, api_errno *err)
{
    if (!fat_dir_valid(des, err))
        return -1;
    *pos = fat_tells[des];
    return 0;
}

int fat_seekdir(int des, int32_t offs, api_errno *err)
{
    if (!fat_dir_valid(des, err))
        return -1;
    if (offs < 0)
    {
        *err = API_EINVAL;
        return -1;
    }
    if (fat_tells[des] > offs)
    {
        FRESULT fresult = f_rewinddir(&fat_dirs[des]);
        if (fresult != FR_OK)
        {
            *err = api_errno_from_fatfs(fresult);
            return -1;
        }
        fat_tells[des] = 0;
    }
    while (fat_tells[des] < offs)
    {
        FILINFO fno;
        FRESULT fresult = f_readdir(&fat_dirs[des], &fno);
        if (fresult != FR_OK)
        {
            *err = api_errno_from_fatfs(fresult);
            return -1;
        }
        fat_tells[des]++;
        if (!fno.fname[0]) // sought past the end
        {
            *err = api_errno_from_fatfs(FR_INVALID_OBJECT);
            return -1;
        }
    }
    return 0;
}

int fat_rewinddir(int des, api_errno *err)
{
    if (!fat_dir_valid(des, err))
        return -1;
    FRESULT fresult = f_rewinddir(&fat_dirs[des]);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    fat_tells[des] = 0;
    return 0;
}

int fat_unlink(const char *path, api_errno *err)
{
    FRESULT fresult = f_unlink(path);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

int fat_rename(const char *oldp, const char *newp, api_errno *err)
{
    FRESULT fresult = f_rename(oldp, newp);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

int fat_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err)
{
    FRESULT fresult = f_chmod(path, attr, mask);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

int fat_utime(const char *path, const FILINFO *fno, api_errno *err)
{
    FRESULT fresult = f_utime(path, fno);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

int fat_mkdir(const char *path, api_errno *err)
{
    FRESULT fresult = f_mkdir(path);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

int fat_chdir(const char *path, api_errno *err)
{
    FRESULT fresult = f_chdir(path);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

int fat_chdrive(const char *path, api_errno *err)
{
    FRESULT fresult = f_chdrive(path);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

int fat_getcwd(char *buf, size_t size, api_errno *err)
{
    FRESULT fresult = f_getcwd(buf, (UINT)size);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

int fat_getlabel(const char *path, char *label, api_errno *err)
{
    DWORD vsn;
    FRESULT fresult = f_getlabel(path, label, &vsn);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

int fat_setlabel(const char *path, api_errno *err)
{
    FRESULT fresult = f_setlabel(path);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

int fat_getfree(const char *path, uint32_t *free_sectors, uint32_t *total_sectors, api_errno *err)
{
    DWORD fre_clust;
    FATFS *fs;
    FRESULT fresult = f_getfree(path, &fre_clust, &fs);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    uint64_t tot = (uint64_t)(fs->n_fatent - 2) * fs->csize;
    uint64_t fre = (uint64_t)fre_clust * fs->csize;
    *total_sectors = tot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)tot;
    *free_sectors = fre > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fre;
    return 0;
}
