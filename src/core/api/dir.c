/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See osal/dir.h.
 */

#include "core/api/dir.h"

#include <string.h>

static int32_t tells[DIR_MAX_OPEN];

static bool dir_push_stat(f_stat_t *info)
{
    /* The xstack grows downward, so the fields are pushed last first to land
     * in struct order for the 6502. */
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

static const char *dir_path(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    return path;
}

static bool dir_return(bool ok, api_errno err)
{
    return ok ? api_return_ax(0) : api_return_errno(err);
}

/* End of directory is an empty name rather than an error, so it does not
 * advance the count. */
static bool dir_next(int des, f_stat_t *info, api_errno *err)
{
    if (!drive_readdir(des, info, err))
        return false;
    if (info->fname[0])
        tells[des]++;
    return true;
}

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

/* A savestate carries an open directory as a path and a count, because the
 * only position a drive here will take is a read per entry: a load reopens
 * the path and reads forward to the count.
 *
 * Under SST_SHARED the path slots are written empty, because the blob crosses
 * to another machine where a path under one peer's home directory names
 * nothing under the other's. Such a load keeps the directories and the
 * working directory it already has, though the entry counts still come from
 * the blob. */
#define DIR_SLOT (API_PATH_MAX + 1)

void dir_sst_save(sst_cursor_t *c, unsigned flags)
{
    for (int i = 0; i < DIR_MAX_OPEN; i++)
        sst_put_i32(c, tells[i]);
    for (int i = 0; i < DIR_MAX_OPEN; i++)
    {
        char path[DIR_SLOT];
        bool open = !(flags & SST_SHARED) && drive_dir_path(i, path, sizeof path);
        sst_put_bool(c, open);
        sst_put_str(c, open ? path : "", DIR_SLOT);
    }
    char cwd[DIR_SLOT];
    api_errno err;
    if (flags & SST_SHARED || !drive_getcwd(cwd, sizeof cwd, &err))
        cwd[0] = 0;
    sst_put_str(c, cwd, DIR_SLOT);
}

bool dir_sst_load(sst_cursor_t *c, unsigned flags)
{
    int32_t at[DIR_MAX_OPEN];
    for (int i = 0; i < DIR_MAX_OPEN; i++)
        at[i] = sst_get_i32(c);
    bool open[DIR_MAX_OPEN];
    char paths[DIR_MAX_OPEN][DIR_SLOT];
    for (int i = 0; i < DIR_MAX_OPEN; i++)
    {
        open[i] = sst_get_bool(c);
        sst_get_str(c, paths[i], DIR_SLOT);
    }
    char cwd[DIR_SLOT];
    sst_get_str(c, cwd, DIR_SLOT);
    if (!sst_ok(c))
        return false;

    /* Only a load with no flags restores the working directory, because
     * drive_chdir moves the whole host process and a caller passing
     * SST_TRUSTED or SST_SHARED shares that process with something else. */
    api_errno err;
    if (!flags && cwd[0] && !drive_chdir(cwd, &err))
        return false;

    for (int i = 0; i < DIR_MAX_OPEN; i++)
    {
        tells[i] = at[i];
        if (flags & SST_SHARED)
            continue;
        if (!open[i])
        {
            if (drive_validate(i, &err))
                drive_closedir(i, &err);
            continue;
        }
        if (!drive_reopendir(i, paths[i], &err))
            return false;
        /* POSIX does not promise a host lists a directory in the same order
         * twice, so this winds to the same count and not to the same
         * entries. */
        f_stat_t info;
        for (int32_t n = 0; n < at[i]; n++)
            if (!drive_readdir(i, &info, &err) || !info.fname[0])
                return false;
    }
    return true;
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
    /* The xstack holds newname, its terminator, then oldname, which is the
     * reverse of the order drive_rename takes them. */
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
    /* All four fields are taken whether or not this drive stores creation
     * times, because the 6502 sent all four: crtime in the registers and the
     * other three on the xstack. */
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
