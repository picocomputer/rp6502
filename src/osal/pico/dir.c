/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's drive: what core/api/dir.c asks a filesystem for, answered
 * by FatFs over its own DIR pool.
 *
 * FatFs and the API keep the same eight fields about an entry, because both
 * of them are FAT's -- but they are two structs, so stat_from_fatfs is where
 * one becomes the other. Every other machine builds the API's directly.
 */

#include "osal/pico/errmap.h"
#include "core/api/api.h"
#include "osal/dir.h"
#include "fatfs/ff.h"
#include <assert.h>
#include <string.h>

// Validate essential settings in ffconf.h
static_assert(FF_LFN_BUF == 255);
static_assert(FF_SFN_BUF == 12);
static_assert(FF_USE_CHMOD == 1);
static_assert(FF_FS_CRTIME == 1);
static_assert(FF_USE_LABEL == 1);
static_assert(FF_LFN_UNICODE == 0);

/* The two names are sized the same on both sides, so a copy is a copy. */
static_assert(FF_LFN_BUF == F_NAME_MAX);
static_assert(FF_SFN_BUF == F_ALTNAME_MAX);

static DIR dirs[DIR_MAX_OPEN];

/* The two paths proc holds, at the length a FatFs path can be. An empty
 * first byte is a free slot. */
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

/* ---- The drive, as core/api/dir.c asks for it ---------------------------- */

/* FatFs reports through a FRESULT, so every one of these is the same shape:
 * make the call, hand back what it said. */
/* FatFs converts filenames through its own active page, so it is told. The
 * volume's names are the drive's business, which is why this lives here. */
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

/* FatFs fills its own record; the API has its own, with the same eight fields
 * because both of them are FAT's. This is the one place they meet. */
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

bool drive_stat(const char *path, f_stat_t *info, api_errno *err)
{
    FILINFO fno;
    if (!fat_ok(f_stat((const TCHAR *)path, &fno), err))
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
    if (!fat_ok(f_opendir(&dirs[i], (const TCHAR *)path), err))
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
    return fat_ok(f_unlink((const TCHAR *)path), err);
}

bool drive_rename(const char *oldname, const char *newname, api_errno *err)
{
    return fat_ok(f_rename((const TCHAR *)oldname, (const TCHAR *)newname), err);
}

bool drive_mkdir(const char *path, api_errno *err)
{
    return fat_ok(f_mkdir((const TCHAR *)path), err);
}

bool drive_chdir(const char *path, api_errno *err)
{
    return fat_ok(f_chdir((const TCHAR *)path), err);
}

bool drive_chdrive(const char *drive, api_errno *err)
{
    return fat_ok(f_chdrive((const TCHAR *)drive), err);
}

bool drive_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err)
{
    return fat_ok(f_chmod((const TCHAR *)path, attr, mask), err);
}

bool drive_utime(const char *path, const f_stat_t *info, api_errno *err)
{
    /* f_utime reads only the four stamps out of what it is given. */
    FILINFO fno = {.fdate = info->fdate,
                   .ftime = info->ftime,
                   .crdate = info->crdate,
                   .crtime = info->crtime};
    return fat_ok(f_utime((const TCHAR *)path, &fno), err);
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
    (void)size; /* f_getlabel writes at most 12 bytes, which is what it is given */
    DWORD vsn;
    return fat_ok(f_getlabel((const TCHAR *)path, (TCHAR *)label, &vsn), err);
}

bool drive_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect,
                            api_errno *err)
{
    DWORD fre_clust;
    FATFS *fs;
    if (!fat_ok(f_getfree((const TCHAR *)path, &fre_clust, &fs), err))
        return false;
    uint64_t tot = (uint64_t)(fs->n_fatent - 2) * fs->csize;
    uint64_t fre = (uint64_t)fre_clust * fs->csize;
    *tot_sect = tot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)tot;
    *fre_sect = fre > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)fre;
    return true;
}

