/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "osal/pico/dir.h"
#include "osal/pico/errmap.h"
#include "core/api/api.h"
#include "core/str/path.h"
#include "osal/dir.h"
#include "fatfs/ff.h"
#include <assert.h>
#include <string.h>

static_assert(FF_LFN_BUF == 255);
static_assert(FF_SFN_BUF == 12);
static_assert(FF_USE_CHMOD == 1);
static_assert(FF_FS_CRTIME == 1);
static_assert(FF_USE_LABEL == 1);
static_assert(FF_LFN_UNICODE == 0);

static_assert(FF_LFN_BUF == F_NAME_MAX);
static_assert(FF_SFN_BUF == F_ALTNAME_MAX);

static DIR dirs[DIR_MAX_OPEN];

/* core/api/proc.c holds at most two paths: what is running and what to
 * return to. */
static char paths[2][FF_LFN_BUF + 1];

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

void oem_fs_code_page(uint16_t cp)
{
    f_setcp(cp);
}

static inline bool fat_ok(FRESULT fresult, api_errno *err)
{
    if (fresult == FR_OK)
        return true;
    *err = fresult_to_api(fresult);
    return false;
}

/* The part of a path after its drive name, which ends at a ':' ahead of any
 * separator. */
static const char *fat_after_drive(const char *path)
{
    const char *end = path + strcspn(path, ":/\\");
    return *end == ':' ? end + 1 : path;
}

bool fat_path_ok(const char *path, api_errno *err)
{
    return path_fat_ok(fat_after_drive(path), true, err);
}

bool fat_names_dir(const char *path)
{
    if (!*fat_after_drive(path))
        return false;
    DIR dir;
    if (f_opendir(&dir, (const TCHAR *)path) != FR_OK)
        return false;
    f_closedir(&dir);
    return true;
}

bool drive_validate(int des, api_errno *err)
{
    if (des < 0 || des >= DIR_MAX_OPEN)
    {
        *err = API_EINVAL;
        return false;
    }
    if (dirs[des].obj.fs == 0)
    {
        *err = API_EBADF;
        return false;
    }
    return true;
}

static void stat_from_fatfs(f_stat_t *info, const FILINFO *fno)
{
    memcpy(info->fname, fno->fname, sizeof info->fname);
    memcpy(info->altname, fno->altname, sizeof info->altname);
    info->fsize = fno->fsize > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fno->fsize;
    info->fattrib = fno->fattrib;
    info->fdate = fno->fdate;
    info->ftime = fno->ftime;
    info->crdate = fno->crdate;
    info->crtime = fno->crtime;
}

/* f_stat refuses a path that resolves to the root of a volume as an invalid
 * name, so the root entry is made here for such a path when it opens as a
 * directory. */
bool drive_stat(const char *path, f_stat_t *info, api_errno *err)
{
    const char *rest = fat_after_drive(path);
    if (!path_fat_ok(rest, true, err))
        return false;
    if (!rest[0])
    {
        *err = f_getldnumber(path) < 0 ? API_ENODEV : API_EINVAL;
        return false;
    }
    FILINFO fno;
    FRESULT fresult = f_stat((const TCHAR *)path, &fno);
    if (fresult == FR_INVALID_NAME && fat_names_dir(path))
    {
        f_stat_root(info);
        return true;
    }
    if (!fat_ok(fresult, err))
        return false;
    stat_from_fatfs(info, &fno);
    return true;
}

bool drive_opendir(const char *path, int *des, api_errno *err)
{
    int i = 0;
    for (; i < DIR_MAX_OPEN; i++)
        if (dirs[i].obj.fs == 0)
            break;
    if (i == DIR_MAX_OPEN)
    {
        *err = API_EMFILE;
        return false;
    }
    if (!fat_path_ok(path, err) ||
        !fat_ok(f_opendir(&dirs[i], (const TCHAR *)path), err))
        return false;
    *des = i;
    return true;
}

bool drive_readdir(int des, f_stat_t *info, api_errno *err)
{
    FILINFO fno;
    if (!fat_ok(f_readdir(&dirs[des], &fno), err))
        return false;
    stat_from_fatfs(info, &fno);
    return true;
}

bool drive_closedir(int des, api_errno *err)
{
    FRESULT fresult = f_closedir(&dirs[des]);
    dirs[des].obj.fs = 0;
    return fat_ok(fresult, err);
}

bool drive_rewinddir(int des, api_errno *err)
{
    return fat_ok(f_rewinddir(&dirs[des]), err);
}

bool drive_unlink(const char *path, api_errno *err)
{
    return fat_path_ok(path, err) && fat_ok(f_unlink((const TCHAR *)path), err);
}

static bool fat_is_file(const char *path)
{
    FILINFO fno;
    return f_stat((const TCHAR *)path, &fno) == FR_OK && !(fno.fattrib & AM_DIR);
}

/* f_rename discards the drive of the new name and renames on the drive of the
 * old one, so names on two drives are refused before anything changes. FatFs
 * also refuses a new name already in use, while rename(2) and MoveFileEx
 * replace a file there, so when both names are files, the file at the new
 * name is removed and the rename is tried again. FAT offers no way to do
 * that atomically. */
bool drive_rename(const char *oldname, const char *newname, api_errno *err)
{
    if (!fat_path_ok(oldname, err) || !fat_path_ok(newname, err))
        return false;
    if (f_getldnumber(oldname) != f_getldnumber(newname))
    {
        *err = API_ENODEV;
        return false;
    }
    FRESULT fr = f_rename((const TCHAR *)oldname, (const TCHAR *)newname);
    if (fr == FR_EXIST && fat_is_file(oldname) && fat_is_file(newname) &&
        (fr = f_unlink((const TCHAR *)newname)) == FR_OK)
        fr = f_rename((const TCHAR *)oldname, (const TCHAR *)newname);
    return fat_ok(fr, err);
}

bool drive_mkdir(const char *path, api_errno *err)
{
    return fat_path_ok(path, err) && fat_ok(f_mkdir((const TCHAR *)path), err);
}

/* f_chdir sets the folder of the drive it names without making that drive
 * current, so the drive is made current after it, as on the other
 * machines. */
bool drive_chdir(const char *path, api_errno *err)
{
    if (!fat_path_ok(path, err) || !fat_ok(f_chdir((const TCHAR *)path), err))
        return false;
    if (strchr(path, ':'))
        f_chdrive((const TCHAR *)path);
    return true;
}

/* A name is empty for the current drive, or a volume ID and its colon with
 * nothing after it, as on the other machines. FatFs itself takes any name
 * without a colon as the current drive and drops whatever follows the colon,
 * so the check comes first. f_chdrive accepts a slot with no drive in it, so
 * f_getlabel mounts the volume first. */
bool drive_chdrive(const char *drive, api_errno *err)
{
    const char *colon = strchr(drive, ':');
    if (drive[0] && (!colon || colon[1]))
    {
        *err = API_ENODEV;
        return false;
    }
    return fat_ok(f_getlabel((const TCHAR *)drive, NULL, NULL), err) &&
           fat_ok(f_chdrive((const TCHAR *)drive), err);
}

bool drive_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err)
{
    return fat_path_ok(path, err) &&
           fat_ok(f_chmod((const TCHAR *)path, attr, mask), err);
}

bool drive_utime(const char *path, const f_stat_t *info, api_errno *err)
{
    /* f_utime reads only fdate, ftime, crdate and crtime out of a FILINFO. */
    FILINFO fno = {.fdate = info->fdate,
                   .ftime = info->ftime,
                   .crdate = info->crdate,
                   .crtime = info->crtime};
    return fat_path_ok(path, err) &&
           fat_ok(f_utime((const TCHAR *)path, &fno), err);
}

bool drive_getcwd(char *buf, size_t size, api_errno *err)
{
    return fat_ok(f_getcwd((TCHAR *)buf, (UINT)size), err);
}

bool drive_setlabel(const char *path, api_errno *err)
{
    return fat_ok(f_setlabel((const TCHAR *)path), err);
}

bool drive_getlabel(const char *path, char *label, size_t size, api_errno *err)
{
    /* FF_FS_EXFAT is RP6502_EXFAT, which host/pico/CMakeLists.txt pins to 0.
     * With it off, f_getlabel writes at most the 12 bytes core/api/dir.c
     * passes. */
    (void)size;
    DWORD vsn;
    return fat_ok(f_getlabel((const TCHAR *)path, (TCHAR *)label, &vsn), err);
}

std_rw_result drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect,
                            api_errno *err)
{
    DWORD fre_clust;
    FATFS *fs;
    if (!fat_ok(f_getfree((const TCHAR *)path, &fre_clust, &fs), err))
        return STD_ERROR;
    /* n_fatent counts entries 0 and 1, which are reserved and hold no data. */
    uint64_t tot = (uint64_t)(fs->n_fatent - 2) * fs->csize;
    uint64_t fre = (uint64_t)fre_clust * fs->csize;
    *tot_sect = tot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)tot;
    *fre_sect = fre > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fre;
    return STD_OK;
}

bool drive_dir_path(int des, char *buf, size_t size)
{
    (void)des, (void)buf, (void)size;
    return false;
}

bool drive_reopendir(int des, const char *path, api_errno *err)
{
    (void)des, (void)path;
    *err = API_ENOSYS;
    return false;
}
