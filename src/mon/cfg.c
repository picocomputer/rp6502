/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cfg.h"
#include "lfs.h"

// Configuration is a plain ASCII file on the LFS. e.g.
// +V1         | Version - Must be first
// +P8000      | PHI2
// +C0         | Caps
// +R0         | RESB
// +S437       | Code Page
// BASIC       | Boot ROM - Must be last

#define CFG_VERSION 1
static const char *filename = "CONFIG.SYS";

void cfg_save()
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, filename,
                                     LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                                     &lfs_file_config);
    if (lfsresult < 0)
    {
        printf("?Unable to lfs_file_opencfg %s for writing (%d)\n", filename, lfsresult);
        return;
    }

    lfsresult = lfs_printf(&lfs_volume, &lfs_file, "Hello %d\n", 42);
    printf("wrote %d bytes\n", lfsresult);

    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfsresult < 0)
        printf("?Unable to lfs_file_close %s (%d)\n", filename, lfsresult);
}

void cfg_load()
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, filename,
                                     LFS_O_RDONLY, &lfs_file_config);
    if (lfsresult < 0)
    {
        printf("?Unable to lfs_file_opencfg %s for reading (%d)\n", filename, lfsresult);
        return;
    }

    char str[80];
    printf("%s\n", lfs_gets(str, 80, &lfs_volume, &lfs_file));

    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfsresult < 0)
        printf("?Unable to lfs_file_close %s (%d)\n", filename, lfsresult);
}
