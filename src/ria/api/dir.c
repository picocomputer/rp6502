/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/dir.h"
#include "api/fat.h"
#include <fatfs/ff.h>
#include <assert.h>
#include <string.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_DIR)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Validate the FILINFO fields dir_push_filinfo marshals to the 6502.
static_assert(FF_LFN_BUF == 255);
static_assert(FF_SFN_BUF == 12);
static_assert(FF_FS_CRTIME == 1);
static_assert(FF_LFN_UNICODE == 0);

void dir_run(void)
{
    fat_dir_run();
}

void dir_stop(void)
{
    fat_dir_stop();
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
bool dir_api_stat(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FILINFO fno;
    api_errno err;
    if (fat_stat(path, &fno, &err) < 0)
        return api_return_errno(err);
    if (!dir_push_filinfo(&fno))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

// int f_opendir (const char* name);
bool dir_api_opendir(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    int des = fat_opendir(path, &err);
    if (des < 0)
        return api_return_errno(err);
    return api_return_ax(des);
}

// int f_readdir (struct f_stat dirent*, int dirdes);
bool dir_api_readdir(void)
{
    FILINFO fno;
    api_errno err;
    if (fat_readdir(API_A, &fno, &err) < 0)
        return api_return_errno(err);
    if (!dir_push_filinfo(&fno))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

// int f_closedir (int dirdes);
bool dir_api_closedir(void)
{
    api_errno err;
    if (fat_closedir(API_A, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

// long f_telldir (int dirdes);
bool dir_api_telldir(void)
{
    int32_t pos;
    api_errno err;
    if (fat_telldir(API_A, &pos, &err) < 0)
        return api_return_errno(err);
    return api_return_axsreg(pos);
}

// int f_seekdir (long offs, int dirdes);
bool dir_api_seekdir(void)
{
    int des = API_A;
    int32_t offs;
    if (!api_pop_int32_end(&offs))
        return api_return_errno(API_EINVAL);
    api_errno err;
    if (fat_seekdir(des, offs, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

// int f_rewinddir (int dirdes);
bool dir_api_rewinddir(void)
{
    api_errno err;
    if (fat_rewinddir(API_A, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

// int unlink(const char* name)
bool dir_api_unlink(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    if (fat_unlink(path, &err) < 0)
        return api_return_errno(err);
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
    api_errno err;
    if (fat_rename((TCHAR *)oldname, (TCHAR *)newname, &err) < 0)
        return api_return_errno(err);
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
    api_errno err;
    if (fat_chmod(path, attr, mask, &err) < 0)
        return api_return_errno(err);
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
    api_errno err;
    if (fat_utime(path, &fno, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

// int f_mkdir(const char* name)
bool dir_api_mkdir(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    if (fat_mkdir(path, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

// int chdir(const char* name)
bool dir_api_chdir(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    if (fat_chdir(path, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

// int f_chdrive(const char* name)
bool dir_api_chdrive(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    if (fat_chdrive(path, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

// int f_getcwd(char* name, int size)
bool dir_api_getcwd(void)
{
    api_errno err;
    if (fat_getcwd((TCHAR *)xstack, XSTACK_SIZE, &err) < 0)
        return api_return_errno(err);
    uint16_t result_len = strlen((char *)xstack);
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
    api_errno err;
    if (fat_setlabel(path, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

// int f_getlabel(const char* path, char* label, unsigned long* vsn)
bool dir_api_getlabel(void)
{
    const int label_size = 12;
    char label[label_size];
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    if (fat_getlabel(path, label, &err) < 0)
        return api_return_errno(err);
    size_t label_len, ret_len;
    label_len = ret_len = strlen(label);
    while (label_len)
        if (!api_push_char(&label[--label_len]))
            return api_return_errno(API_ENOMEM);
    return api_return_ax(ret_len + 1);
}

// int f_getfree(const char* name, unsigned long* free, unsigned long* total)
bool dir_api_getfree(void)
{
    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    uint32_t fre_sect, tot_sect;
    api_errno err;
    if (fat_getfree(path, &fre_sect, &tot_sect, &err) < 0)
        return api_return_errno(err);
    if (!api_push_uint32(&tot_sect) || !api_push_uint32(&fre_sect))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}
