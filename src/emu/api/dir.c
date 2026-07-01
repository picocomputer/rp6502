/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502 directory / path syscalls (ria/api/dir.c) marshalled to a swappable
 * filesystem backend: g_dir_ops is the native host (host_dir_ops) by default, or
 * a real FatFs (fat_dir_ops) while --tmpdrive is mounted — the drive lifecycle
 * swaps it. The xstack marshalling matches the firmware so the same 6502 binaries
 * work; the ops fill a FatFs FILINFO the enumeration syscalls marshal byte-for-byte.
 */

#include "emu/api/api.h"
#include "emu/host/dir.h"
#include "emu/sys/mem.h"
#include "api/api.h"
#include "api/dir.h"
#include <string.h>

/* The active filesystem backend (host by default; the ramdrive swaps in FatFs). */
static const fs_dir_ops *g_dir_ops = &host_dir_ops;

void emu_set_dir_ops(const fs_dir_ops *ops)
{
    g_dir_ops = ops;
}

void emu_dir_stop(void)
{
    g_dir_ops->stop();
}

/* Push one entry in the firmware FILINFO byte order: fields reversed so they
 * land forward in the 6502-visible struct (mirrors firmware dir_push_filinfo). */
static bool dir_push_filinfo(FILINFO *fno)
{
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

bool dir_api_stat(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    FILINFO fno;
    api_errno err;
    if (g_dir_ops->stat(path, &fno, &err) < 0)
        return api_return_errno(err);
    if (!dir_push_filinfo(&fno))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

bool dir_api_opendir(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    int des = g_dir_ops->opendir(path, &err);
    if (des < 0)
        return api_return_errno(err);
    return api_return_ax((uint16_t)des);
}

bool dir_api_readdir(void)
{
    FILINFO fno;
    api_errno err;
    if (g_dir_ops->readdir(API_A, &fno, &err) < 0)
        return api_return_errno(err);
    if (!dir_push_filinfo(&fno))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

bool dir_api_closedir(void)
{
    api_errno err;
    if (g_dir_ops->closedir(API_A, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool dir_api_telldir(void)
{
    int32_t pos;
    api_errno err;
    if (g_dir_ops->telldir(API_A, &pos, &err) < 0)
        return api_return_errno(err);
    return api_return_axsreg((uint32_t)pos);
}

bool dir_api_seekdir(void)
{
    int des = API_A;
    int32_t offs;
    if (!api_pop_int32_end(&offs))
        return api_return_errno(API_EINVAL);
    api_errno err;
    if (g_dir_ops->seekdir(des, offs, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool dir_api_rewinddir(void)
{
    api_errno err;
    if (g_dir_ops->rewinddir(API_A, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool dir_api_chmod(void)
{
    uint8_t mask = API_A;
    uint8_t attr;
    if (!api_pop_uint8(&attr))
        return api_return_errno(API_EINVAL);
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    if (g_dir_ops->chmod(path, attr, mask, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool dir_api_utime(void)
{
    /* AX = crtime; then crdate, ftime, fdate off the xstack — the firmware order.
     * Fill a FILINFO so the FatFs backend (f_utime) sets creation + modification
     * time; the host backend applies only the modification date/time. */
    FILINFO fno;
    fno.crtime = (uint16_t)(API_A | (API_X << 8));
    if (!api_pop_uint16(&fno.crdate) || !api_pop_uint16(&fno.ftime) || !api_pop_uint16(&fno.fdate))
        return api_return_errno(API_EINVAL);
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    if (g_dir_ops->utime(path, &fno, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool dir_api_setlabel(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    if (g_dir_ops->setlabel(path, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool dir_api_getlabel(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    char label[12];
    api_errno err;
    if (g_dir_ops->getlabel(path, label, &err) < 0)
        return api_return_errno(err);
    size_t len = strlen(label);
    for (size_t i = len; i;)
        if (!api_push_char(&label[--i]))
            return api_return_errno(API_ENOMEM);
    return api_return_ax((uint16_t)(len + 1));
}

bool dir_api_getfree(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    uint32_t fre, tot;
    api_errno err;
    if (g_dir_ops->getfree(path, &fre, &tot, &err) < 0)
        return api_return_errno(err);
    if (!api_push_uint32(&tot) || !api_push_uint32(&fre))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

bool dir_api_unlink(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    if (g_dir_ops->unlink(path, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool dir_api_rename(void)
{
    /* xstack holds newname\0oldname (firmware order); rename(old, new). */
    char *newname = (char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    char *oldname = newname;
    while (*oldname)
        oldname++;
    if (oldname == (char *)&xstack[XSTACK_SIZE])
        return api_return_errno(API_EINVAL);
    oldname++;
    api_errno err;
    if (g_dir_ops->rename(oldname, newname, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool dir_api_mkdir(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    if (g_dir_ops->mkdir(path, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool dir_api_chdir(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    if (g_dir_ops->chdir(path, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool dir_api_chdrive(void)
{
    const char *drive = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    api_errno err;
    if (g_dir_ops->chdrive(drive, &err) < 0)
        return api_return_errno(err);
    return api_return_ax(0);
}

bool dir_api_getcwd(void)
{
    /* Write the cwd at the bottom of the xstack, then relocate it to the top so
     * it pops in order — matching firmware dir_api_getcwd. */
    api_errno err;
    if (g_dir_ops->getcwd((char *)xstack, XSTACK_SIZE, &err) < 0)
    {
        xstack_ptr = XSTACK_SIZE;
        return api_return_errno(err);
    }
    uint16_t len = (uint16_t)strlen((char *)xstack);
    xstack_ptr = XSTACK_SIZE;
    for (uint16_t i = len; i;)
        xstack[--xstack_ptr] = xstack[--i];
    return api_return_ax((uint16_t)(len + 1));
}
