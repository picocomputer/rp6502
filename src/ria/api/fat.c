/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The FatFs filesystem module: the stdio file driver (open/close/read/write/lseek/
 * sync over a FIL pool, extracted from usb/msc.c) and the file/directory management
 * API (the 0x1B..0x2E syscalls, over a DIR pool). FatFs-only — the block device
 * (diskio) is provided by the platform, so the desktop emulator reuses this over a
 * RAM disk.
 */

#include "ria/api/api.h"
#include "ria/api/fat.h"
#include "fatfs/ff.h"
#include <assert.h>
#include <pico.h>
#include <stdio.h>
#include <string.h>

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

/* ---- File and directory management (the 0x1B..0x2E syscalls) -------------- */

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_FAT)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Validate essential settings in ffconf.h
static_assert(FF_LFN_BUF == 255);
static_assert(FF_SFN_BUF == 12);
static_assert(FF_USE_CHMOD == 1);
static_assert(FF_FS_CRTIME == 1);
static_assert(FF_USE_LABEL == 1);
static_assert(FF_LFN_UNICODE == 0);

#define DIR_MAX_OPEN 8
static DIR dirs[DIR_MAX_OPEN];
static int32_t tells[DIR_MAX_OPEN];

// Failure returns -1 and sets errno from FatFS FRESULT
static inline bool api_return_fresult(unsigned fresult)
{
    return api_return_errno(fat_fresult_to_api_errno(fresult));
}

void fat_run(void)
{
    for (int i = 0; i < DIR_MAX_OPEN; i++)
        dirs[i].obj.fs = 0;
}

void fat_stop(void)
{
    for (int i = 0; i < DIR_MAX_OPEN; i++)
    {
        f_closedir(&dirs[i]);
        dirs[i].obj.fs = 0;
    }
}

static bool dir_push_filinfo(FILINFO *fno)
{
    // Push fields in reverse so they land in forward
    // order in the 6502-visible struct.
    bool ok = true;
    for (int i = FF_LFN_BUF; i >= 0; i--)
        ok &= api_push_char(&fno->fname[i]);
    for (int i = FF_SFN_BUF; i >= 0; i--)
        ok &= api_push_char(&fno->altname[i]);
    ok &= api_push_uint8(&fno->fattrib);
    ok &= api_push_uint16(&fno->crtime);
    ok &= api_push_uint16(&fno->crdate);
    ok &= api_push_uint16(&fno->ftime);
    ok &= api_push_uint16(&fno->fdate);
    uint32_t fsize = fno->fsize;
    if (fno->fsize > 0xFFFFFFFF)
        fsize = 0xFFFFFFFF;
    ok &= api_push_uint32(&fsize);
    return ok;
}

// int f_stat (const char *path, struct f_stat *dirent);
bool fat_api_stat(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FILINFO fno;
    FRESULT fresult = f_stat(path, &fno);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    if (!dir_push_filinfo(&fno))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

// int f_opendir (const char* name);
bool fat_api_opendir(void)
{
    DIR *dir = 0;
    unsigned des = 0;
    for (; des < DIR_MAX_OPEN; des++)
        if (dirs[des].obj.fs == 0)
        {
            dir = &dirs[des];
            break;
        }
    if (!dir)
        return api_return_errno(API_EMFILE);
    tells[des] = 0;
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_opendir(dir, path);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(des);
}

// int f_readdir (struct f_stat dirent*, int dirdes);
bool fat_api_readdir(void)
{
    unsigned des = API_A;
    if (des >= DIR_MAX_OPEN)
        return api_return_errno(API_EINVAL);
    DIR *dir = &dirs[des];
    if (dir->obj.fs == 0)
        return api_return_errno(API_EBADF);
    FILINFO fno;
    FRESULT fresult = f_readdir(dir, &fno);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    if (fno.fname[0])
        tells[des]++;
    if (!dir_push_filinfo(&fno))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

// int f_closedir (int dirdes);
bool fat_api_closedir(void)
{
    unsigned des = API_A;
    if (des >= DIR_MAX_OPEN)
        return api_return_errno(API_EINVAL);
    DIR *dir = &dirs[des];
    if (dir->obj.fs == 0)
        return api_return_errno(API_EBADF);
    FRESULT fresult = f_closedir(dir);
    dir->obj.fs = 0;
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// long f_telldir (int dirdes);
bool fat_api_telldir(void)
{
    unsigned des = API_A;
    if (des >= DIR_MAX_OPEN)
        return api_return_errno(API_EINVAL);
    DIR *dir = &dirs[des];
    if (dir->obj.fs == 0)
        return api_return_errno(API_EBADF);
    return api_return_axsreg(tells[des]);
}

// int f_seekdir (long offs, int dirdes);
bool fat_api_seekdir(void)
{
    unsigned des = API_A;
    if (des >= DIR_MAX_OPEN)
        return api_return_errno(API_EINVAL);
    DIR *dir = &dirs[des];
    if (dir->obj.fs == 0)
        return api_return_errno(API_EBADF);
    int32_t offs;
    if (!api_pop_int32_end(&offs))
        return api_return_errno(API_EINVAL);
    if (offs < 0)
        return api_return_errno(API_EINVAL);
    if (tells[des] > offs)
    {
        FRESULT fresult = f_rewinddir(dir);
        if (fresult != FR_OK)
            return api_return_fresult(fresult);
        tells[des] = 0;
    }
    while (tells[des] < offs)
    {
        FILINFO fno;
        FRESULT fresult = f_readdir(dir, &fno);
        if (fresult != FR_OK)
            return api_return_fresult(fresult);
        tells[des]++;
        if (!fno.fname[0])
            return api_return_fresult(FR_INVALID_OBJECT);
    }
    return api_return_ax(0);
}

// int f_rewinddir (int dirdes);
bool fat_api_rewinddir(void)
{
    unsigned des = API_A;
    if (des >= DIR_MAX_OPEN)
        return api_return_errno(API_EINVAL);
    DIR *dir = &dirs[des];
    if (dir->obj.fs == 0)
        return api_return_errno(API_EBADF);
    FRESULT fresult = f_rewinddir(dir);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    tells[des] = 0;
    return api_return_ax(0);
}

// int unlink(const char* name)
bool fat_api_unlink(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_unlink(path);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// int rename(const char* oldname, const char* newname)
bool fat_api_rename(void)
{
    uint8_t *oldname, *newname;
    oldname = newname = &xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    while (*oldname)
        oldname++;
    if (oldname == &xstack[XSTACK_SIZE])
        return api_return_errno(API_EINVAL);
    oldname++;
    FRESULT fresult = f_rename((TCHAR *)oldname, (TCHAR *)newname);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// int f_chmod (const char *path, unsigned char attr,  unsigned char mask);
bool fat_api_chmod(void)
{
    uint8_t mask = API_A;
    uint8_t attr;
    if (!api_pop_uint8(&attr))
        return api_return_errno(API_EINVAL);
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_chmod(path, attr, mask);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// int f_utime (const char *path, unsigned fdate, unsigned ftime, unsigned crdate, unsigned crtime);
bool fat_api_utime(void)
{
    FILINFO fno;
    fno.crtime = API_AX;
    if (!api_pop_uint16(&fno.crdate) ||
        !api_pop_uint16(&fno.ftime) ||
        !api_pop_uint16(&fno.fdate))
        return api_return_errno(API_EINVAL);
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_utime(path, &fno);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// int f_mkdir(const char* name)
bool fat_api_mkdir(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_mkdir(path);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// int chdir(const char* name)
bool fat_api_chdir(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_chdir(path);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// int f_chdrive(const char* name)
bool fat_api_chdrive(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_chdrive(path);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// int f_getcwd(char* name, int size)
bool fat_api_getcwd(void)
{
    FRESULT fresult = f_getcwd((TCHAR *)xstack, XSTACK_SIZE);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    uint16_t result_len = strlen((char *)xstack);
    xstack_ptr = XSTACK_SIZE;
    for (uint16_t i = result_len; i;)
        xstack[--xstack_ptr] = xstack[--i];
    return api_return_ax(result_len + 1);
}

// int f_setlabel(const char* name)
bool fat_api_setlabel(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_setlabel(path);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// int f_getlabel(const char* path, char* label, unsigned long* vsn)
bool fat_api_getlabel(void)
{
    char label[12];
    DWORD vsn;
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_getlabel(path, label, &vsn);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    size_t label_len, ret_len;
    label_len = ret_len = strlen(label);
    while (label_len)
        if (!api_push_char(&label[--label_len]))
            return api_return_errno(API_ENOMEM);
    return api_return_ax(ret_len + 1);
}

// int f_getfree(const char* name, unsigned long* free, unsigned long* total)
bool fat_api_getfree(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    DWORD fre_clust;
    FATFS *fs;
    FRESULT fresult = f_getfree(path, &fre_clust, &fs);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    uint64_t tot = (uint64_t)(fs->n_fatent - 2) * fs->csize;
    uint64_t fre = (uint64_t)fre_clust * fs->csize;
    uint32_t tot_sect = tot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)tot;
    uint32_t fre_sect = fre > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fre;
    if (!api_push_uint32(&tot_sect) || !api_push_uint32(&fre_sect))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}
