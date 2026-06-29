/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502 directory / path syscalls (ria/api/dir.c) routed to the host-backed
 * MSC0: drive (mscdir.c). The xstack marshalling matches the firmware so the same
 * 6502 binaries work; the operations themselves act on real host files instead
 * of FatFs. fs_info_t carries one entry in the firmware FILINFO field set so
 * the enumeration ops marshal it to the 6502 byte-for-byte (dir_push_info).
 */

#include "emu/api/api.h"
#include "emu/msc/mscdir.h"
#include "emu/msc/mscpath.h"
#include "emu/sys/mem.h"
#include "api/api.h"
#include "api/dir.h"
#include <errno.h>
#include <string.h>

/* Push one entry in the firmware FILINFO byte order: fields reversed so they
 * land forward in the 6502-visible struct (mirrors firmware dir_push_filinfo). */
static bool dir_push_info(fs_info_t *info)
{
    bool ok = true;
    for (int i = 255; i >= 0; i--)
        ok &= api_push_char(&info->name[i]);
    for (int i = 12; i >= 0; i--)
        ok &= api_push_char(&info->altname[i]);
    ok &= api_push_uint8(&info->attrib);
    ok &= api_push_uint16(&info->ctime);
    ok &= api_push_uint16(&info->cdate);
    ok &= api_push_uint16(&info->mtime);
    ok &= api_push_uint16(&info->mdate);
    ok &= api_push_uint32(&info->size);
    return ok;
}

bool dir_api_stat(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    fs_info_t info;
    if (fs_stat(path, &info) != 0)
        return api_return_errno(api_errno_from_host(errno));
    if (!dir_push_info(&info))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

bool dir_api_opendir(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    int des = fs_opendir(path);
    if (des < 0)
        return api_return_errno(api_errno_from_host(errno));
    return api_return_ax((uint16_t)des);
}

bool dir_api_readdir(void)
{
    fs_info_t info;
    if (fs_readdir(API_A, &info) != 0)
        return api_return_errno(api_errno_from_host(errno));
    if (!dir_push_info(&info))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

bool dir_api_closedir(void)
{
    if (fs_closedir(API_A) != 0)
        return api_return_errno(api_errno_from_host(errno));
    return api_return_ax(0);
}

bool dir_api_telldir(void)
{
    long pos = fs_telldir(API_A);
    if (pos < 0)
        return api_return_errno(api_errno_from_host(errno));
    return api_return_axsreg((uint32_t)pos);
}

bool dir_api_seekdir(void)
{
    int des = API_A;
    int32_t offs;
    if (!api_pop_int32_end(&offs))
        return api_return_errno(API_EINVAL);
    if (fs_seekdir(des, offs) != 0)
        return api_return_errno(api_errno_from_host(errno));
    return api_return_ax(0);
}

bool dir_api_rewinddir(void)
{
    if (fs_rewinddir(API_A) != 0)
        return api_return_errno(api_errno_from_host(errno));
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
    if (fs_chmod(path, attr, mask) != 0)
        return api_return_errno(api_errno_from_host(errno));
    return api_return_ax(0);
}

bool dir_api_utime(void)
{
    /* AX = crtime; then crdate, ftime, fdate off the xstack. Only the
     * modification date/time reaches the host (see fs_utime). */
    uint16_t crdate, ftime, fdate;
    if (!api_pop_uint16(&crdate) || !api_pop_uint16(&ftime) || !api_pop_uint16(&fdate))
        return api_return_errno(API_EINVAL);
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    if (fs_utime(path, fdate, ftime) != 0)
        return api_return_errno(api_errno_from_host(errno));
    return api_return_ax(0);
}

bool dir_api_setlabel(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    if (fs_setlabel(path) != 0)
        return api_return_errno(api_errno_from_host(errno));
    return api_return_ax(0);
}

bool dir_api_getlabel(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    char label[16];
    if (fs_getlabel(path, label, sizeof(label)) != 0)
        return api_return_errno(api_errno_from_host(errno));
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
    if (fs_getfree(path, &fre, &tot) != 0)
        return api_return_errno(api_errno_from_host(errno));
    if (!api_push_uint32(&tot) || !api_push_uint32(&fre))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

bool dir_api_unlink(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    if (fs_unlink(path) != 0)
        return api_return_errno(api_errno_from_host(errno));
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
    if (fs_rename(oldname, newname) != 0)
        return api_return_errno(api_errno_from_host(errno));
    return api_return_ax(0);
}

bool dir_api_mkdir(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    if (fs_mkdir(path) != 0)
        return api_return_errno(api_errno_from_host(errno));
    return api_return_ax(0);
}

bool dir_api_chdir(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    if (fs_chdir(path) != 0)
        return api_return_errno(api_errno_from_host(errno));
    return api_return_ax(0);
}

bool dir_api_chdrive(void)
{
    const char *drive = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    if (fs_chdrive(drive) != 0)
        return api_return_errno(api_errno_from_host(errno));
    return api_return_ax(0);
}

bool dir_api_getcwd(void)
{
    /* Write the cwd at the bottom of the xstack, then relocate it to the top
     * so it pops in order — matching firmware dir_api_getcwd. */
    size_t len = fs_getcwd((char *)xstack, XSTACK_SIZE);
    if (len == 0) /* did not fit the xstack — f_getcwd is full-path-or-error */
    {
        xstack_ptr = XSTACK_SIZE;
        return api_return_errno(API_ENOMEM);
    }
    xstack_ptr = XSTACK_SIZE;
    for (size_t i = len; i;)
        xstack[--xstack_ptr] = xstack[--i];
    return api_return_ax((uint16_t)(len + 1));
}
