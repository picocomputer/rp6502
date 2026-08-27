/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's drive: what core/api/dir.c asks a filesystem for, answered
 * by FatFs over its own DIR pool.
 */

#include "api/dir.h"
#include "api/errno.h"
#include "core/api/api.h"
#include "core/api/dir.h"
#include "fatfs/ff.h"
#include "host.h" /* HOST_IN_FLASH */
#include <assert.h>

// Validate essential settings in ffconf.h
static_assert(FF_LFN_BUF == 255);
static_assert(FF_SFN_BUF == 12);
static_assert(FF_USE_CHMOD == 1);
static_assert(FF_FS_CRTIME == 1);
static_assert(FF_USE_LABEL == 1);
static_assert(FF_LFN_UNICODE == 0);

static DIR dirs[DIR_MAX_OPEN];

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

static bool fat_dir_validate(int des, api_errno *err)
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

static bool fat_dir_stat(const char *path, FILINFO *fno, api_errno *err)
{
    return fat_ok(f_stat((const TCHAR *)path, fno), err);
}

static bool fat_dir_opendir(const char *path, int *des, api_errno *err)
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

static bool fat_dir_readdir(int des, FILINFO *fno, api_errno *err)
{
    return fat_ok(f_readdir(&dirs[des], fno), err);
}

static bool fat_dir_closedir(int des, api_errno *err)
{
    FRESULT fresult = f_closedir(&dirs[des]);
    dirs[des].obj.fs = 0;
    return fat_ok(fresult, err);
}

static bool fat_dir_rewinddir(int des, api_errno *err)
{
    return fat_ok(f_rewinddir(&dirs[des]), err);
}

static bool fat_dir_unlink(const char *path, api_errno *err)
{
    return fat_ok(f_unlink((const TCHAR *)path), err);
}

static bool fat_dir_rename(const char *oldname, const char *newname, api_errno *err)
{
    return fat_ok(f_rename((const TCHAR *)oldname, (const TCHAR *)newname), err);
}

static bool fat_dir_mkdir(const char *path, api_errno *err)
{
    return fat_ok(f_mkdir((const TCHAR *)path), err);
}

static bool fat_dir_chdir(const char *path, api_errno *err)
{
    return fat_ok(f_chdir((const TCHAR *)path), err);
}

static bool fat_dir_chdrive(const char *drive, api_errno *err)
{
    return fat_ok(f_chdrive((const TCHAR *)drive), err);
}

static bool fat_dir_chmod(const char *path, uint8_t attr, uint8_t mask, api_errno *err)
{
    return fat_ok(f_chmod((const TCHAR *)path, attr, mask), err);
}

static bool fat_dir_utime(const char *path, const FILINFO *fno, api_errno *err)
{
    return fat_ok(f_utime((const TCHAR *)path, fno), err);
}

static bool fat_dir_getcwd(char *buf, size_t size, api_errno *err)
{
    return fat_ok(f_getcwd((TCHAR *)buf, (UINT)size), err);
}

static bool fat_dir_setlabel(const char *path, api_errno *err)
{
    return fat_ok(f_setlabel((const TCHAR *)path), err);
}

static bool fat_dir_getlabel(const char *path, char *label, size_t size, api_errno *err)
{
    (void)size; /* f_getlabel writes at most 12 bytes, which is what it is given */
    DWORD vsn;
    return fat_ok(f_getlabel((const TCHAR *)path, (TCHAR *)label, &vsn), err);
}

static bool fat_dir_getfree(const char *path, uint32_t *tot_sect, uint32_t *fre_sect,
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

const dir_backend_t HOST_IN_FLASH("fat_dir") fat_dir_backend = {
    .stat = fat_dir_stat,
    .unlink = fat_dir_unlink,
    .rename = fat_dir_rename,
    .mkdir = fat_dir_mkdir,
    .chdir = fat_dir_chdir,
    .chdrive = fat_dir_chdrive,
    .chmod = fat_dir_chmod,
    .utime = fat_dir_utime,
    .getfree = fat_dir_getfree,
    .getcwd = fat_dir_getcwd,
    .getlabel = fat_dir_getlabel,
    .setlabel = fat_dir_setlabel,
    .opendir = fat_dir_opendir,
    .readdir = fat_dir_readdir,
    .closedir = fat_dir_closedir,
    .rewinddir = fat_dir_rewinddir,
    .validate = fat_dir_validate,
};
