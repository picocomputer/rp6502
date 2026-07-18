/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Real FatFs on the emulator's RAM disk (host/fat.c diskio) — the same FatFs the
 * firmware runs. Formats a fresh volume and exercises the core paths (mkfs,
 * mount, open/write/read, directory enumeration, mkdir/chdir/getcwd) so the
 * shared filesystem code is covered on the host. Lays a path for running the
 * 6502 filesystem syscalls over a real FatFs (--tmpdrive) rather than the host.
 */

#include "api/oem.h"
#include "str/str.h"
#include "emu/host/tmp.h"
#include "fatfs/ff.h"
#include "utest.h"
#include <stdbool.h>
#include <string.h>

static FATFS g_fs;
static BYTE g_work[4096]; /* f_mkfs work area (>= FF_MAX_SS) */

/* Wipe the RAM disk, format a fresh FAT volume, and mount it (default drive). */
static bool mounted(void)
{
    tmp_disk_reset();
    if (f_mkfs("", 0, g_work, sizeof(g_work)) != FR_OK)
        return false;
    return f_mount(&g_fs, "", 1) == FR_OK;
}

UTEST(fatfs, mkfs_and_mount)
{
    ASSERT_TRUE(mounted());
    DWORD nclust = 0;
    FATFS *fs = NULL;
    ASSERT_EQ(f_getfree("", &nclust, &fs), FR_OK);
    ASSERT_GT(nclust, 0u); /* a formatted volume has free clusters */
    f_unmount("");
}

UTEST(fatfs, write_read_back)
{
    ASSERT_TRUE(mounted());

    FIL fp;
    UINT bw = 0, br = 0;
    ASSERT_EQ(f_open(&fp, "hello.txt", FA_CREATE_NEW | FA_WRITE), FR_OK);
    ASSERT_EQ(f_write(&fp, "hello", 5, &bw), FR_OK);
    ASSERT_EQ(bw, 5u);
    ASSERT_EQ(f_close(&fp), FR_OK);

    char buf[8] = {0};
    ASSERT_EQ(f_open(&fp, "hello.txt", FA_READ), FR_OK);
    ASSERT_EQ(f_read(&fp, buf, sizeof(buf), &br), FR_OK);
    ASSERT_EQ(br, 5u);
    ASSERT_STREQ(buf, "hello");
    ASSERT_EQ(f_close(&fp), FR_OK);

    /* CREATE_NEW on an existing file fails (the write actually persisted). */
    ASSERT_EQ(f_open(&fp, "hello.txt", FA_CREATE_NEW | FA_WRITE), FR_EXIST);
    f_unmount("");
}

UTEST(fatfs, dir_enum_and_chdir)
{
    ASSERT_TRUE(mounted());

    FIL fp;
    UINT bw = 0;
    ASSERT_EQ(f_open(&fp, "a.txt", FA_CREATE_NEW | FA_WRITE), FR_OK);
    f_write(&fp, "x", 1, &bw);
    f_close(&fp);
    ASSERT_EQ(f_mkdir("sub"), FR_OK);

    DIR dir;
    FILINFO fno;
    bool saw_file = false, saw_dir = false;
    ASSERT_EQ(f_opendir(&dir, ""), FR_OK);
    for (;;)
    {
        ASSERT_EQ(f_readdir(&dir, &fno), FR_OK);
        if (!fno.fname[0])
            break;
        if (!strcmp(fno.fname, "a.txt"))
            saw_file = true;
        if (!strcmp(fno.fname, "sub") && (fno.fattrib & AM_DIR))
            saw_dir = true;
    }
    f_closedir(&dir);
    ASSERT_TRUE(saw_file);
    ASSERT_TRUE(saw_dir);

    ASSERT_EQ(f_chdir("sub"), FR_OK);
    char cwd[64];
    ASSERT_EQ(f_getcwd(cwd, sizeof(cwd)), FR_OK);
    ASSERT_TRUE(strstr(cwd, "sub") != NULL);
    f_unmount("");
}

/* Non-ASCII (OEM code page) names. FF_CODE_PAGE=0 means the conversion tables
 * come from f_setcp, which the oem module seeds — without it every name byte
 * >= 0x80 fails FR_INVALID_NAME. */
UTEST(fatfs, oem_names)
{
    str_init(); /* apply the default locale, seeding f_setcp (437) */
    ASSERT_TRUE(mounted());

    FIL fp;
    UINT bw = 0;
    ASSERT_EQ(f_open(&fp, "caf\x82.txt", FA_CREATE_NEW | FA_WRITE), FR_OK); /* CP437 'é' */
    ASSERT_EQ(f_write(&fp, "x", 1, &bw), FR_OK);
    ASSERT_EQ(f_close(&fp), FR_OK);

    DIR dir;
    FILINFO fno;
    bool saw = false;
    ASSERT_EQ(f_opendir(&dir, ""), FR_OK);
    for (;;)
    {
        ASSERT_EQ(f_readdir(&dir, &fno), FR_OK);
        if (!fno.fname[0])
            break;
        if (!strcmp(fno.fname, "caf\x82.txt"))
            saw = true;
    }
    f_closedir(&dir);
    ASSERT_TRUE(saw); /* the OEM bytes round-trip through the UTF-16 LFN */

    ASSERT_EQ(f_stat("caf\x82.txt", &fno), FR_OK);
    ASSERT_EQ(f_unlink("caf\x82.txt"), FR_OK);
    f_unmount("");
}

UTEST_MAIN()
