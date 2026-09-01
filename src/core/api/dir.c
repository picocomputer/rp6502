/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See osal/dir.h.
 */

#include "core/api/dir.h"

#include <string.h>

/* How many entries each open directory has handed out. telldir reports it,
 * seekdir winds to it, rewinddir zeroes it -- and no drive needs to know,
 * because a directory read is a directory read. */
static int32_t tells[DIR_MAX_OPEN];

static bool dir_push_stat(f_stat_t *info)
{
    /* Push fields in reverse so they land in forward
     * order in the 6502-visible struct. */
    bool ok = true;
    for (int i = F_NAME_MAX; i >= 0; i--)
        ok &= api_push_char(&info->fname[i]);
    for (int i = F_ALTNAME_MAX; i >= 0; i--)
        ok &= api_push_char(&info->altname[i]);
    ok &= api_push_uint8(&info->fattrib);
    ok &= api_push_uint16(&info->crtime);
    ok &= api_push_uint16(&info->crdate);
    ok &= api_push_uint16(&info->ftime);
    ok &= api_push_uint16(&info->fdate);
    ok &= api_push_uint32(&info->fsize);
    return ok;
}

/* The path a whole-path op was called with, consuming the xstack. */
static const char *dir_path(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    return path;
}

/* What a backend said, as the 6502 sees it. */
static bool dir_return(bool ok, api_errno err)
{
    return ok ? api_return_ax(0) : api_return_errno(err);
}

/* Read one entry through the backend and count it. End-of-directory is an
 * empty name, not an error, and does not advance the counter. */
static bool dir_next(int des, f_stat_t *info, api_errno *err)
{
    if (!drive_readdir(des, info, err))
        return false;
    if (info->fname[0])
        tells[des]++;
    return true;
}

/* Nothing stays open across a machine event: a run starts with none, and a
 * stop leaves none behind. */
void dir_run(void)
{
    dir_stop();
    for (int i = 0; i < DIR_MAX_OPEN; i++)
        tells[i] = 0;
}

void dir_stop(void)
{
    api_errno err;
    for (int i = 0; i < DIR_MAX_OPEN; i++)
        if (drive_validate(i, &err))
            drive_closedir(i, &err);
}

bool dir_api_stat(void)
{
    f_stat_t info;
    api_errno err;
    if (!drive_stat(dir_path(), &info, &err))
        return api_return_errno(err);
    if (!dir_push_stat(&info))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

bool dir_api_opendir(void)
{
    int des;
    api_errno err;
    if (!drive_opendir(dir_path(), &des, &err))
        return api_return_errno(err);
    tells[des] = 0;
    return api_return_ax((uint16_t)des);
}

bool dir_api_readdir(void)
{
    int des = API_A;
    api_errno err;
    if (!drive_validate(des, &err))
        return api_return_errno(err);
    f_stat_t info;
    if (!dir_next(des, &info, &err))
        return api_return_errno(err);
    if (!dir_push_stat(&info))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}

bool dir_api_closedir(void)
{
    int des = API_A;
    api_errno err;
    if (!drive_validate(des, &err))
        return api_return_errno(err);
    return dir_return(drive_closedir(des, &err), err);
}

bool dir_api_telldir(void)
{
    int des = API_A;
    api_errno err;
    if (!drive_validate(des, &err))
        return api_return_errno(err);
    return api_return_axsreg((uint32_t)tells[des]);
}

/* Seek by entry index: wind back to the start if the target is behind, then
 * read forward to it. Past the end is EINVAL -- there is no such entry to be
 * positioned at. */
bool dir_api_seekdir(void)
{
    int des = API_A;
    int32_t offs;
    if (!api_pop_int32_end(&offs))
        return api_return_errno(API_EINVAL);
    api_errno err;
    if (!drive_validate(des, &err))
        return api_return_errno(err);
    if (offs < 0)
        return api_return_errno(API_EINVAL);
    if (tells[des] > offs)
    {
        if (!drive_rewinddir(des, &err))
            return api_return_errno(err);
        tells[des] = 0;
    }
    while (tells[des] < offs)
    {
        f_stat_t info;
        if (!dir_next(des, &info, &err))
            return api_return_errno(err);
        if (!info.fname[0])
            return api_return_errno(API_EINVAL);
    }
    return api_return_ax(0);
}

bool dir_api_rewinddir(void)
{
    int des = API_A;
    api_errno err;
    if (!drive_validate(des, &err))
        return api_return_errno(err);
    if (!drive_rewinddir(des, &err))
        return api_return_errno(err);
    tells[des] = 0;
    return api_return_ax(0);
}

bool dir_api_unlink(void)
{
    api_errno err;
    return dir_return(drive_unlink(dir_path(), &err), err);
}

bool dir_api_rename(void)
{
    /* The xstack holds newname\0oldname; rename takes them the other way. */
    const char *newname = dir_path();
    const char *oldname = newname;
    while (*oldname)
        oldname++;
    if (oldname == (const char *)&xstack[XSTACK_SIZE])
        return api_return_errno(API_EINVAL);
    oldname++;
    api_errno err;
    return dir_return(drive_rename(oldname, newname, &err), err);
}

bool dir_api_chmod(void)
{
    uint8_t mask = API_A;
    uint8_t attr;
    if (!api_pop_uint8(&attr))
        return api_return_errno(API_EINVAL);
    api_errno err;
    return dir_return(drive_chmod(dir_path(), attr, mask, &err), err);
}

bool dir_api_utime(void)
{
    /* All four are popped whether or not this drive stores creation times,
     * because the 6502 pushed all four. */
    f_stat_t info;
    info.crtime = API_AX;
    if (!api_pop_uint16(&info.crdate) ||
        !api_pop_uint16(&info.ftime) ||
        !api_pop_uint16(&info.fdate))
        return api_return_errno(API_EINVAL);
    api_errno err;
    return dir_return(drive_utime(dir_path(), &info, &err), err);
}

bool dir_api_mkdir(void)
{
    api_errno err;
    return dir_return(drive_mkdir(dir_path(), &err), err);
}

bool dir_api_chdir(void)
{
    api_errno err;
    return dir_return(drive_chdir(dir_path(), &err), err);
}

bool dir_api_chdrive(void)
{
    api_errno err;
    return dir_return(drive_chdrive(dir_path(), &err), err);
}

/* The drive writes the path at the bottom of the xstack; it is relocated to
 * the top so the 6502 pops it in order. */
bool dir_api_getcwd(void)
{
    api_errno err;
    bool ok = drive_getcwd((char *)xstack, XSTACK_SIZE, &err);
    xstack_ptr = XSTACK_SIZE;
    if (!ok)
        return api_return_errno(err);
    uint16_t len = (uint16_t)strlen((char *)xstack);
    for (uint16_t i = len; i;)
        xstack[--xstack_ptr] = xstack[--i];
    return api_return_ax(len + 1);
}

bool dir_api_setlabel(void)
{
    api_errno err;
    return dir_return(drive_setlabel(dir_path(), &err), err);
}

bool dir_api_getlabel(void)
{
    char label[12];
    api_errno err;
    if (!drive_getlabel(dir_path(), label, sizeof(label), &err))
        return api_return_errno(err);
    size_t len = strlen(label);
    for (size_t i = len; i;)
        if (!api_push_char(&label[--i]))
            return api_return_errno(API_ENOMEM);
    return api_return_ax((uint16_t)(len + 1));
}

bool dir_api_getfree(void)
{
    uint32_t tot_sect, fre_sect;
    api_errno err;
    if (!drive_getfree(dir_path(), &tot_sect, &fre_sect, &err))
        return api_return_errno(err);
    if (!api_push_uint32(&tot_sect) || !api_push_uint32(&fre_sect))
        return api_return_errno(API_ENOMEM);
    return api_return_ax(0);
}
