/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fs.h"

#include "core/api/dir.h"

#include <string.h>

static char paths[2][API_PATH_MAX + 1];

char *os_dir_path_hold(const char *path)
{
    size_t len = strlen(path);
    for (size_t i = 0; i < 2; i++)
        if (!paths[i][0] && len < sizeof paths[i])
        {
            memcpy(paths[i], path, len + 1);
            return paths[i];
        }
    return NULL;
}

void os_dir_path_drop(char *path)
{
    path[0] = '\0';
}

bool drive_getcwd(char *buf, size_t size, api_errno *err)
{
    static const char cwd[] = FS_DRIVE FS_ASSETS_PATH;
    if (size < sizeof cwd)
    {
        *err = API_ENOMEM;
        return false;
    }
    memcpy(buf, cwd, sizeof cwd);
    return true;
}

bool drive_chdrive(const char *drive, api_errno *err)
{
    if (!*fs_strip_drive(drive))
        return true;
    *err = API_ENODEV;
    return false;
}

static bool drive_enosys(api_errno *err)
{
    *err = API_ENOSYS;
    return false;
}

bool drive_stat(const char *path, f_stat_t *info, api_errno *err)
{
    (void)path, (void)info;
    return drive_enosys(err);
}

bool drive_unlink(const char *path, api_errno *err)
{
    (void)path;
    return drive_enosys(err);
}

bool drive_rename(const char *oldname, const char *newname, api_errno *err)
{
    (void)oldname, (void)newname;
    return drive_enosys(err);
}

bool drive_mkdir(const char *path, api_errno *err)
{
    (void)path;
    return drive_enosys(err);
}

bool drive_chdir(const char *path, api_errno *err)
{
    (void)path;
    return drive_enosys(err);
}

bool drive_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err)
{
    (void)path, (void)attr, (void)mask;
    return drive_enosys(err);
}

bool drive_utime(const char *path, const f_stat_t *info, api_errno *err)
{
    (void)path, (void)info;
    return drive_enosys(err);
}

std_rw_result drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect, api_errno *err)
{
    (void)path, (void)tot_sect, (void)fre_sect;
    *err = API_ENOSYS;
    return STD_ERROR;
}

bool drive_getlabel(const char *path, char *label, size_t size, api_errno *err)
{
    (void)path, (void)label, (void)size;
    return drive_enosys(err);
}

bool drive_setlabel(const char *path, api_errno *err)
{
    (void)path;
    return drive_enosys(err);
}

bool drive_opendir(const char *path, int *des, api_errno *err)
{
    (void)path, (void)des;
    return drive_enosys(err);
}

bool drive_readdir(int des, f_stat_t *info, api_errno *err)
{
    (void)des, (void)info;
    return drive_enosys(err);
}

bool drive_closedir(int des, api_errno *err)
{
    (void)des;
    return drive_enosys(err);
}

bool drive_rewinddir(int des, api_errno *err)
{
    (void)des;
    return drive_enosys(err);
}

bool drive_validate(int des, api_errno *err)
{
    (void)des;
    return drive_enosys(err);
}

bool drive_dir_path(int des, char *buf, size_t size)
{
    (void)des, (void)buf, (void)size;
    return false;
}

bool drive_reopendir(int des, const char *path, api_errno *err)
{
    (void)des, (void)path;
    return drive_enosys(err);
}
