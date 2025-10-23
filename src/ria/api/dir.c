/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/dir.h"
#include <fatfs/ff.h>
#include <pico.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_DIR)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Validate essential settings in ffconf.h
static_assert(FF_LFN_BUF == 255);
static_assert(FF_SFN_BUF == 12);
static_assert(FF_USE_CHMOD == 1);
static_assert(FF_FS_CRTIME == 1);
static_assert(FF_USE_LABEL == 1);

#define DIR_MAX_OPEN 8
static DIR dirs[DIR_MAX_OPEN];
static int32_t tells[DIR_MAX_OPEN];

void dir_run(void)
{
    for (int i = 0; i < DIR_MAX_OPEN; i++)
        dirs[i].obj.fs = 0;
}

void dir_stop(void)
{
    for (int i = 0; i < DIR_MAX_OPEN; i++)
        f_closedir(&dirs[i]);
}

static void dir_push_filinfo(FILINFO *fno)
{
    // Ensure 6502 struct never changes and
    // always looks like FSIZE_t = 32-bits.
    for (int i = FF_LFN_BUF; i >= 0; i--)
        api_push_char(&fno->fname[i]);
    for (int i = FF_SFN_BUF; i >= 0; i--)
        api_push_char(&fno->altname[i]);
    api_push_uint8(&fno->fattrib);
    api_push_uint16(&fno->crtime);
    api_push_uint16(&fno->crdate);
    api_push_uint16(&fno->ftime);
    api_push_uint16(&fno->fdate);
    uint32_t fsize = fno->fsize;
    if (fno->fsize > 0xFFFFFFFF)
        fsize = 0xFFFFFFFF;
    api_push_uint32(&fsize);
}

// int f_stat (const char *path, struct f_stat *dirent);
bool dir_api_stat(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FILINFO fno;
    FRESULT fresult = f_stat(path, &fno);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    dir_push_filinfo(&fno);
    return api_return_ax(0);
}

// int f_opendir (const char* name);
bool dir_api_opendir(void)
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
bool dir_api_readdir(void)
{
    unsigned des = API_A;
    if (des >= DIR_MAX_OPEN)
        return api_return_errno(API_EINVAL);
    DIR *dir = &dirs[des];
    FILINFO fno;
    FRESULT fresult = f_readdir(dir, &fno);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    if (fno.fname[0])
        tells[des]++;
    dir_push_filinfo(&fno);
    return api_return_ax(0);
}

// int f_closedir (int dirdes);
bool dir_api_closedir(void)
{
    unsigned des = API_A;
    if (des >= DIR_MAX_OPEN)
        return api_return_errno(API_EINVAL);
    DIR *dir = &dirs[des];
    FRESULT fresult = f_closedir(dir);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// long f_telldir (int dirdes);
bool dir_api_telldir(void)
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
bool dir_api_seekdir(void)
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
bool dir_api_rewinddir(void)
{
    unsigned des = API_A;
    if (des >= DIR_MAX_OPEN)
        return api_return_errno(API_EINVAL);
    DIR *dir = &dirs[des];
    FRESULT fresult = f_rewinddir(dir);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    tells[des] = 0;
    return api_return_ax(0);
}

// int unlink(const char* name)
bool dir_api_unlink(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_unlink(path);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// int rename(const char* oldname, const char* newname)
bool dir_api_rename(void)
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
bool dir_api_chmod(void)
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
bool dir_api_utime(void)
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
bool dir_api_mkdir(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_mkdir(path);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// int chdir(const char* name)
bool dir_api_chdir(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_chdir(path);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// int f_chdrive(const char* name)
bool dir_api_chdrive(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_chdrive(path);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// int f_getcwd(char* name, int size)
bool dir_api_getcwd(void)
{
    FRESULT fresult = f_getcwd((TCHAR *)xstack, XSTACK_SIZE);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    uint16_t result_len = strlen((char *)xstack);
    // relocate
    xstack_ptr = XSTACK_SIZE;
    for (uint16_t i = result_len; i;)
        xstack[--xstack_ptr] = xstack[--i];
    return api_return_ax(result_len + 1);
}

// int f_setlabel(const char* name)
bool dir_api_setlabel(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_setlabel(path);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// int f_getlabel(const char* path, char* label)
bool dir_api_getlabel(void)
{
    // The FatFs docs say to use 23.
    // Windows and Linux have an 11 char limit.
    // TODO Figure out why it's not 12.
    const int label_size = 23;
    char label[label_size];
    DWORD vsn;
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_getlabel(path, label, &vsn);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    size_t label_len, ret_len;
    label_len = ret_len = strlen(label);
    // This should never happen.
    if (label_len > 11)
        return api_return_errno(API_ERANGE);
    while (label_len)
        api_push_char(&label[--label_len]);
    return api_return_ax(ret_len + 1);
}

// int f_getfree(const char* name, unsigned long* free, unsigned long* total)
bool dir_api_getfree(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    DWORD fre_clust, fre_sect, tot_sect;
    FATFS *fs;
    FRESULT fresult = f_getfree(path, &fre_clust, &fs);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    tot_sect = (fs->n_fatent - 2) * fs->csize;
    fre_sect = fre_clust * fs->csize;
    api_push_uint32(&tot_sect);
    api_push_uint32(&fre_sect);
    return api_return_ax(0);
}
