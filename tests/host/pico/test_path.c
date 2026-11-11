/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* path_eq compares paths as FAT does, on a FAT disk in RAM: a card names the
 * running ROM whatever the case of its directories or the short name of its
 * drive. argv[0] is recorded the way rom_exec records it. */

#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "sys/path.h"
#include "utest.h"
#include <string.h>

static BYTE disk[2880 * 512];
DSTATUS disk_initialize(BYTE p) { return p ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE p) { return p ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE p, BYTE *b, LBA_t s, UINT c)
{
    (void)p;
    memcpy(b, disk + s * 512, c * 512);
    return RES_OK;
}
DRESULT disk_write(BYTE p, const BYTE *b, LBA_t s, UINT c)
{
    (void)p;
    memcpy(disk + s * 512, b, c * 512);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE p, BYTE cmd, void *b)
{
    (void)p;
    if (cmd == GET_SECTOR_COUNT)
        *(LBA_t *)b = 2880;
    if (cmd == GET_SECTOR_SIZE)
        *(WORD *)b = 512;
    if (cmd == GET_BLOCK_SIZE)
        *(DWORD *)b = 1;
    return RES_OK;
}
DWORD get_fattime(void) { return 0; }
int drive_mkfs_capture(BYTE t, DWORD a)
{
    (void)t;
    (void)a;
    return 0;
}
const char *VolumeStr[FF_VOLUMES] = {"MSC0", "MSC1", "MSC2", "MSC3", "MSC4",
                                     "MSC5", "MSC6", "MSC7", "MSC8", "MSC9"};

/* core/str/str.c's, at code page 437, which links too much of the RIA. */
WCHAR ff_oem2uni(WCHAR, WORD);
DWORD ff_wtoupper(DWORD);
bool str_oem_eq(const char *a, const char *b)
{
    for (;; a++, b++)
    {
        if (ff_wtoupper(ff_oem2uni((unsigned char)*a, 437)) !=
            ff_wtoupper(ff_oem2uni((unsigned char)*b, 437)))
            return false;
        if (!*a)
            return true;
    }
}

static void mount(void)
{
    static FATFS fs;
    static bool done;
    if (done)
        return;
    BYTE work[512];
    MKFS_PARM opt = {FM_FAT, 0, 0, 0, 0};
    f_setcp(437);
    f_mkfs("MSC0:", &opt, work, sizeof work);
    f_mount(&fs, "MSC0:", 1);
    f_mkdir("MSC0:/Games");
    FIL f;
    f_open(&f, "MSC0:/Games/Jigsaw.rp6502", FA_WRITE | FA_CREATE_ALWAYS);
    f_close(&f);
    f_chdrive("MSC0:");
    done = true;
}

/* argv[0] as rom_exec records a path typed at LOAD. */
static const char *argv0(const char *typed)
{
    static char out[256];
    strcpy(out, path_abs(typed));
    path_correct_basename(out, sizeof out);
    return out;
}

/* A card's path as proc_nfc resolves it. */
static const char *card(const char *text)
{
    static char out[256];
    strcpy(out, path_abs(text));
    return out;
}

UTEST(path, the_case_of_a_directory_does_not_matter)
{
    mount();
    ASSERT_TRUE(path_eq(card("MSC0:/Games/Jigsaw.rp6502"), argv0("/games/jigsaw.rp6502")));
    ASSERT_TRUE(path_eq(card("MSC0:/games/jigsaw.rp6502"), argv0("/GAMES/JIGSAW.RP6502")));
    ASSERT_TRUE(path_eq(card("MSC0://Games/./x/../Jigsaw.rp6502"), argv0("/Games/Jigsaw.rp6502")));
}

UTEST(path, a_short_drive_name_is_the_same_drive)
{
    mount();
    ASSERT_TRUE(path_eq(card("MSC0:/Games/Jigsaw.rp6502"), argv0("0:/Games/Jigsaw.rp6502")));
    ASSERT_TRUE(path_eq(card("0:/Games/Jigsaw.rp6502"), argv0("/Games/Jigsaw.rp6502")));
    ASSERT_FALSE(path_eq(card("MSC1:/Games/Jigsaw.rp6502"), argv0("/Games/Jigsaw.rp6502")));
}

UTEST(path, another_file_is_not_the_same)
{
    mount();
    ASSERT_FALSE(path_eq(card("MSC0:/Games/Jigsaw2.rp6502"), argv0("/Games/Jigsaw.rp6502")));
    ASSERT_FALSE(path_eq(card("MSC0:/Games/Jigsaw.rp6502"), ""));
}

UTEST(path, installed_roms_compare_by_name)
{
    ASSERT_TRUE(path_eq(":JIGSAW", ":JIGSAW"));
    ASSERT_FALSE(path_eq(":JIGSAW", "MSC0:/JIGSAW"));
}

UTEST_MAIN();
