/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/dir.h"

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_DIR)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

void dir_run(void)
{
}

void dir_stop(void)
{
}

// DIR* __fastcall__ opendir (const char* name);
bool dir_api_opendir(void)
{
    return api_return_errno(API_ENOSYS);
}

// struct dirent* __fastcall__ readdir (DIR* dir);
bool dir_api_readdir(void)
{
    return api_return_errno(API_ENOSYS);
}

// int __fastcall__ closedir (DIR* dir);
bool dir_api_closedir(void)
{
    return api_return_errno(API_ENOSYS);
}

// long __fastcall__ telldir (DIR* dir);
bool dir_api_telldir(void)
{
    return api_return_errno(API_ENOSYS);
}

// void __fastcall__ seekdir (DIR* dir, long offs);
bool dir_api_seekdir(void)
{
    return api_return_errno(API_ENOSYS);
}

// void __fastcall__ rewinddir (DIR* dir);
bool dir_api_rewinddir(void)
{
    return api_return_errno(API_ENOSYS);
}
