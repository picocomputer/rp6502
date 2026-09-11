/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "osal/pico/errmap.h"
#include "osal/fs.h"
#include "fatfs/ff.h"
#include "osal/pico/lfs.h"
#include <assert.h>
#include <stdio.h> /* SEEK_SET/CUR/END */
#include <string.h>

// There are eight FatFs slots a program can be handed, plus two descriptors
// that only fs_rom_open returns. fs_std_open never counts that high, so a
// program is never given one. The ROM read descriptor is littlefs when
// the path begins with ':' -- the null drive, where installed ROMs live
// beside the system's own files -- and FatFs otherwise. The ROM write
// descriptor is always littlefs.
#define FAT_FIL_MAX 8
#define FS_DESC_ROM FAT_FIL_MAX
#define FS_DESC_ROM_WR (FAT_FIL_MAX + 1)
static FIL fat_fil_pool[FAT_FIL_MAX + 1];
static bool rom_rd_lfs_live;
static lfs_file_t rom_rd_lfs;
LFS_FILE_CONFIG(rom_rd_lfs_cfg, static);
static bool rom_wr_live;
static lfs_file_t rom_wr_lfs;
LFS_FILE_CONFIG(rom_wr_lfs_cfg, static);

static FIL *fat_validate_fil(int desc)
{
    if (desc < 0 || desc > FS_DESC_ROM)
        return NULL;
    if (!fat_fil_pool[desc].obj.fs)
        return NULL;
    return &fat_fil_pool[desc];
}

int fs_rom_open(const char *path, uint8_t flags, api_errno *err)
{
    if (flags == (FS_WR | FS_CREAT | FS_EXCL))
    {
        if (path[0] != ':')
        {
            *err = API_EINVAL;
            return -1;
        }
        assert(!rom_wr_live);
        int lfsresult = lfs_file_opencfg(&lfs_volume, &rom_wr_lfs, path + 1,
                                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL,
                                         &rom_wr_lfs_cfg);
        if (lfsresult < 0)
        {
            *err = lfs_error_to_api(lfsresult);
            return -1;
        }
        rom_wr_live = true;
        return FS_DESC_ROM_WR;
    }
    if (flags != FS_RD)
    {
        *err = API_EINVAL;
        return -1;
    }
    assert(!rom_rd_lfs_live && !fat_fil_pool[FS_DESC_ROM].obj.fs);
    if (path[0] == ':')
    {
        int lfsresult = lfs_file_opencfg(&lfs_volume, &rom_rd_lfs, path + 1,
                                         LFS_O_RDONLY, &rom_rd_lfs_cfg);
        if (lfsresult < 0)
        {
            *err = lfs_error_to_api(lfsresult);
            return -1;
        }
        rom_rd_lfs_live = true;
        return FS_DESC_ROM;
    }
    FRESULT fresult = f_open(&fat_fil_pool[FS_DESC_ROM], path, FA_READ);
    if (fresult != FR_OK)
    {
        *err = fresult_to_api(fresult);
        return -1;
    }
    return FS_DESC_ROM;
}

bool fs_rom_remove(const char *name, api_errno *err)
{
    if (name[0] == ':')
        name++;
    int lfsresult = lfs_remove(&lfs_volume, name);
    if (lfsresult < 0)
    {
        *err = lfs_error_to_api(lfsresult);
        return false;
    }
    return true;
}

static bool rom_lfs_file(int desc, lfs_file_t **file)
{
    if (desc == FS_DESC_ROM && rom_rd_lfs_live)
    {
        *file = &rom_rd_lfs;
        return true;
    }
    if (desc == FS_DESC_ROM_WR && rom_wr_live)
    {
        *file = &rom_wr_lfs;
        return true;
    }
    return false;
}

bool fs_std_handles(const char *path)
{
    (void)path;
    return true; // catch-all, registered last
}

int fs_std_open(const char *path, uint8_t flags, api_errno *err)
{
    // The low two bits of the public flags are FA_READ and FA_WRITE, so the
    // access mode passes straight through to f_open.
    static_assert(FA_READ == 0x01);
    static_assert(FA_WRITE == 0x02);
    const uint8_t RDWR = 0x03;
    const uint8_t CREAT = 0x10;
    const uint8_t TRUNC = 0x20;
    const uint8_t APPEND = 0x40;
    const uint8_t EXCL = 0x80;

    // TRUNC and APPEND are applied after the open rather than folded into the
    // mode, because POSIX open() honors them with or without CREAT.
    uint8_t mode = flags & RDWR;
    if (flags & CREAT)
        mode |= (flags & EXCL) ? FA_CREATE_NEW : FA_OPEN_ALWAYS;
    // FA_OPEN_EXISTING is 0, so with no CREAT a missing file fails FR_NO_FILE.

    FIL *fp = NULL;
    for (int i = 0; i < FAT_FIL_MAX; i++)
    {
        if (!fat_fil_pool[i].obj.fs)
        {
            fp = &fat_fil_pool[i];
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
        *err = fresult_to_api(fresult);
        return -1;
    }
    FRESULT post = FR_OK;
    if ((flags & TRUNC) && (mode & FA_WRITE))
        post = f_truncate(fp); // truncates to nothing, the offset being 0 after open
    if (post == FR_OK && (flags & APPEND))
        post = f_lseek(fp, f_size(fp));
    if (post != FR_OK)
    {
        f_close(fp);
        fp->obj.fs = NULL;
        *err = fresult_to_api(post);
        return -1;
    }

    return (int)(fp - fat_fil_pool);
}

void fs_std_settle(void)
{
}

bool fs_std_ident(int desc, sst_cursor_t *c)
{
    (void)desc, (void)c;
    return false;
}

int fs_std_reopen(sst_cursor_t *c, api_errno *err)
{
    (void)c;
    *err = API_ENOSYS;
    return -1;
}

std_rw_result fs_std_close(int desc, api_errno *err)
{
    lfs_file_t *lf;
    if (rom_lfs_file(desc, &lf))
    {
        int lfsresult = lfs_file_close(&lfs_volume, lf);
        if (desc == FS_DESC_ROM)
            rom_rd_lfs_live = false;
        else
            rom_wr_live = false;
        if (lfsresult < 0)
        {
            *err = lfs_error_to_api(lfsresult);
            return STD_ERROR;
        }
        return STD_OK;
    }
    FIL *fp = fat_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    FRESULT fresult = f_close(fp);
    fp->obj.fs = NULL;
    if (fresult != FR_OK)
    {
        *err = fresult_to_api(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}

std_rw_result fs_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    lfs_file_t *lf;
    if (rom_lfs_file(desc, &lf))
    {
        lfs_ssize_t lfsresult = lfs_file_read(&lfs_volume, lf, buf, count);
        if (lfsresult < 0)
        {
            *bytes_read = 0;
            *err = lfs_error_to_api((int)lfsresult);
            return STD_ERROR;
        }
        *bytes_read = (uint32_t)lfsresult;
        return STD_OK;
    }
    FIL *fp = fat_validate_fil(desc);
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
        *err = fresult_to_api(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}

std_rw_result fs_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    lfs_file_t *lf;
    if (rom_lfs_file(desc, &lf))
    {
        lfs_ssize_t lfsresult = lfs_file_write(&lfs_volume, lf, buf, count);
        if (lfsresult < 0)
        {
            *bytes_written = 0;
            *err = lfs_error_to_api((int)lfsresult);
            return STD_ERROR;
        }
        *bytes_written = (uint32_t)lfsresult;
        return STD_OK;
    }
    FIL *fp = fat_validate_fil(desc);
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
        *err = fresult_to_api(fresult);
        return STD_ERROR;
    }
    /* FatFs has no disk-full result. f_write stops when it cannot get a
     * cluster and reports the short count, so on a full volume it writes
     * nothing and still returns FR_OK. Nothing written of something asked for
     * is ENOSPC here. */
    if (count && !bw)
    {
        *err = API_ENOSPC;
        return STD_ERROR;
    }
    return STD_OK;
}

int fs_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err)
{
    lfs_file_t *lf;
    if (rom_lfs_file(desc, &lf))
    {
        int lfs_whence = whence == SEEK_SET   ? LFS_SEEK_SET
                         : whence == SEEK_CUR ? LFS_SEEK_CUR
                         : whence == SEEK_END ? LFS_SEEK_END
                                              : -1;
        if (lfs_whence < 0)
        {
            *err = API_EINVAL;
            return -1;
        }
        /* littlefs refuses a position past LFS_FILE_MAX, 0x7FFFFFFF, so what
         * it returns always fits *pos; the FatFs path below has to check. */
        lfs_soff_t lfsresult = lfs_file_seek(&lfs_volume, lf, offset, lfs_whence);
        if (lfsresult < 0)
        {
            *err = lfs_error_to_api((int)lfsresult);
            return -1;
        }
        *pos = (int32_t)lfsresult;
        return 0;
    }
    FIL *fp = fat_validate_fil(desc);
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
    // *pos comes back as a signed 32-bit value, 0xFFFFFFFF being the error
    // sentinel, so a target past 2GB-1 is refused before the seek rather than
    // moving the file pointer somewhere that cannot be reported.
    if (absolute_offset > 0x7FFFFFFF)
    {
        *err = API_ERANGE;
        return -1;
    }
    FRESULT fresult = f_lseek(fp, absolute_offset);
    if (fresult != FR_OK)
    {
        *err = fresult_to_api(fresult);
        return -1;
    }
    *pos = (int32_t)f_tell(fp);
    return 0;
}

std_rw_result fs_std_sync(int desc, api_errno *err)
{
    lfs_file_t *lf;
    if (rom_lfs_file(desc, &lf))
    {
        int lfsresult = lfs_file_sync(&lfs_volume, lf);
        if (lfsresult < 0)
        {
            *err = lfs_error_to_api(lfsresult);
            return STD_ERROR;
        }
        return STD_OK;
    }
    FIL *fp = fat_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    FRESULT fresult = f_sync(fp);
    if (fresult != FR_OK)
    {
        *err = fresult_to_api(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}
