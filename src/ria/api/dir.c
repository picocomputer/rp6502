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

#define DIR_MAX_OPEN 8
static DIR dirs[DIR_MAX_OPEN];

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
    api_push_uint16(&fno->ftime);
    api_push_uint16(&fno->fdate);
    uint32_t fsize = fno->fsize;
    if (fno->fsize > 0xFFFFFFFF)
        fsize = 0xFFFFFFFF;
    api_push_uint32(&fsize);
}

// DIR* __fastcall__ opendir (const char* name);
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
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FRESULT fresult = f_opendir(dir, path);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(des);
}

// struct dirent* __fastcall__ readdir (DIR* dir);
bool dir_api_readdir(void)
{
    unsigned des = API_A;
    if (des >= DIR_MAX_OPEN)
        api_return_errno(API_EINVAL);
    DIR *dir = &dirs[des];
    FILINFO fno;
    FRESULT fresult = f_readdir(dir, &fno);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    dir_push_filinfo(&fno);
    return api_return_ax(0);
}

// int __fastcall__ closedir (DIR* dir);
bool dir_api_closedir(void)
{
    unsigned des = API_A;
    if (des >= DIR_MAX_OPEN)
        api_return_errno(API_EINVAL);
    DIR *dir = &dirs[des];
    FRESULT fresult = f_closedir(dir);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

// long __fastcall__ telldir (DIR* dir);
bool dir_api_telldir(void)
{
    unsigned des = API_A;
    if (des >= DIR_MAX_OPEN)
        api_return_errno(API_EINVAL);
    DIR *dir = &dirs[des];

    if (dir->obj.fs == 0) // TODO
        return api_return_errno(API_EBADF);

    return api_return_errno(API_ENOSYS);
}

// void __fastcall__ seekdir (DIR* dir, long offs);
bool dir_api_seekdir(void)
{
    unsigned des = API_A;
    if (des >= DIR_MAX_OPEN)
        api_return_errno(API_EINVAL);
    DIR *dir = &dirs[des];

    if (dir->obj.fs == 0) // TODO
        return api_return_errno(API_EBADF);

    return api_return_errno(API_ENOSYS);
}

// void __fastcall__ rewinddir (DIR* dir);
bool dir_api_rewinddir(void)
{
    unsigned des = API_A;
    if (des >= DIR_MAX_OPEN)
        api_return_errno(API_EINVAL);
    DIR *dir = &dirs[des];
    FRESULT fresult = f_rewinddir(dir);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_errno(API_ENOSYS);
}
