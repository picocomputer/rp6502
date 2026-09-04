/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's drive, as osal/dir.h asks for it -- which is almost
 * nothing. There are no directories on an APF data slot: a program opens a
 * name and the host binds a slot to it. Two of the seventeen calls can be
 * answered without a directory at all; the other fifteen say ENOSYS, each in
 * its own words, and the linker drops what the machine never reaches.
 *
 * The files themselves are next door in fs.c, which owns the slot pool and
 * the bridge this shares a drive letter with.
 */

#include "fs.h"

#include "core/api/dir.h"

#include <string.h>

/* The two paths proc holds. Short of the host's 256-byte field because this
 * is static RAM; an empty first byte is a free slot. */
static char paths[2][128];

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

/* ---- What this drive can answer ------------------------------------------ */

/* Synthetic: the host cannot be asked. Spelled from the drive so
 * appending a name opens the same file the bare name does. */
bool drive_getcwd(char *buf, size_t size, api_errno *err)
{
    static const char cwd[] = "MSC0:/Saves/rp6502/common/";
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
    const char *rest = fs_strip_drive(drive);
    if (!rest || *rest)
    {
        *err = API_ENODEV;
        return false;
    }
    return true;
}

/* ---- And what it cannot -------------------------------------------------- */

/* One folder with no directories to walk and no metadata to read. These are
 * not stubs waiting to be filled: a data slot has nowhere to put a directory,
 * so ENOSYS is the true answer and not a placeholder for one. */
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

bool drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect, api_errno *err)
{
    (void)path, (void)tot_sect, (void)fre_sect;
    return drive_enosys(err);
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

/* Nothing opens, so nothing is ever a valid descriptor -- and dir_stop walks
 * the pool asking, so this has to answer rather than refuse. */
bool drive_validate(int des, api_errno *err)
{
    (void)des;
    *err = API_EBADF;
    return false;
}
