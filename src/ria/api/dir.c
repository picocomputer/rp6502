/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/dir.h"
#include <fatfs/ff.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_DIR)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

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

// DIR* __fastcall__ opendir (const char* name);
bool dir_api_opendir(void)
{
    DIR *dir = 0;
    int des = 0;
    for (; des < DIR_MAX_OPEN; des++)
        if (dirs[des].obj.fs == 0)
            dir = &dirs[des];
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

    if (dir->obj.fs == 0) // TODO
        return api_return_errno(API_EBADF);

    return api_return_errno(API_ENOSYS);
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
