/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/oem.h"
#include "core/str/str.h"
#include "core/api/dir.h"
#include "core/api/std.h"
#include "core/rom/rom.h"
#include "osal/fs.h"
#include "core/ria/regs.h"
#include "host/host.h"
#include "osal/dir.h"
#include "osal/os.h"
#include "dirsys.h"
#include "stdsys.h"
#include "tb_hostos.h"
#include "utest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define O_RD 0x01
#define O_WR 0x02
#define O_CREAT_ 0x10
#define O_TRUNC_ 0x20
#define O_APPEND_ 0x40
#define O_EXCL_ 0x80

static char g_dir[256];

static bool drive_chdir_to(const char *path)
{
    api_errno err;
    return drive_chdir(path, &err);
}

static bool drive_cwd(char *buf, size_t sz)
{
    api_errno err;
    return drive_getcwd(buf, sz, &err);
}

static bool drive_mkdir_at(const char *path)
{
    api_errno err;
    return drive_mkdir(path, &err);
}

/* Option 2 is API_ERRNO_OPT_LLVM, selected so that errno checks can
 * distinguish one error from another. SAVE: is set to the new folder, as a
 * ROM start sets it when the host has no save folder. */
static bool fresh_cwd(void)
{
    char dir[TEST_PATH_MAX];
    if (!host_make_tmpdir(dir, sizeof(dir)))
        return false;
    std_stop();
    if (!drive_chdir_to(dir) || !drive_cwd(g_dir, sizeof(g_dir)))
        return false;
    fs_save_start();
    api_set_errno_opt(2);
    return true;
}

/* g_dir as a host path, without the FS: that the POSIX GETCWD puts in
 * front. */
static const char *host_dir(void)
{
    return strncmp(g_dir, "FS:", 3) ? g_dir : g_dir + 3;
}

static bool host_exists(const char *rel)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", host_dir(), rel);
    FILE *f = fopen(p, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

static void msc_expect(char *out, size_t sz, const char *suffix)
{
    snprintf(out, sz, "%s%s", g_dir, suffix);
}

static void make_file(const char *rel, const char *data, uint16_t n)
{
    int f = ssys_open(rel, O_WR | O_CREAT_ | O_TRUNC_);
    if (f >= 0)
    {
        ssys_write(f, data, n);
        ssys_close(f);
    }
}

#define AM_RDO 0x01
#define AM_DIR 0x10
#define AM_ARC 0x20


UTEST(fs, drive_write_read_seek)
{
    ASSERT_TRUE(fresh_cwd());

    int f = ssys_open("hello.txt", O_WR | O_CREAT_ | O_TRUNC_);
    ASSERT_TRUE(f >= 0);
    ASSERT_EQ(ssys_write(f, "hello", 5), 5);
    ssys_close(f);
    ASSERT_TRUE(host_exists("hello.txt"));

    f = ssys_open("hello.txt", O_RD);
    ASSERT_TRUE(f >= 0);
    char buf[8] = {0};
    ASSERT_EQ(ssys_read(f, buf, 8), 5);
    ASSERT_STREQ(buf, "hello");
    ASSERT_EQ(ssys_lseek(f, 1, SEEK_SET), 1);
    ASSERT_EQ(ssys_read(f, buf, 1), 1);
    ASSERT_EQ(buf[0], 'e');
    ssys_close(f);

    char named[64];
    snprintf(named, sizeof(named), "%shello.txt", host_drive());
    f = ssys_open(named, O_RD);
    ASSERT_TRUE(f >= 0);
    ssys_close(f);

    dsys_path("hello.txt");
    dir_api_unlink();
    ASSERT_EQ(dsys_ax(), 0);
    ASSERT_FALSE(host_exists("hello.txt"));
}

UTEST(fs, chdir_getcwd_relative)
{
    ASSERT_TRUE(fresh_cwd());

    char cwd[TEST_PATH_MAX], expect[TEST_PATH_MAX];
    dir_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    msc_expect(expect, sizeof(expect), "");
    ASSERT_STREQ(cwd, expect);
    ASSERT_EQ(strncmp(cwd, host_drive(), strlen(host_drive())), 0);

    dsys_path("saves");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("saves");
    dir_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    dir_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    msc_expect(expect, sizeof(expect), "/saves");
    ASSERT_STREQ(cwd, expect);

    int f = ssys_open("game.sav", O_WR | O_CREAT_ | O_TRUNC_);
    ASSERT_TRUE(f >= 0);
    ssys_close(f);
    ASSERT_TRUE(host_exists("saves/game.sav"));

    dsys_path("nope");
    dir_api_chdir();
    ASSERT_EQ(dsys_ax(), -1);
}

UTEST(fs, no_chroot_clamp)
{
    ASSERT_TRUE(fresh_cwd());

    dsys_path("sub");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("sub");
    dir_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    char cwd[TEST_PATH_MAX], expect[TEST_PATH_MAX];
    dir_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    msc_expect(expect, sizeof(expect), "/sub");
    ASSERT_STREQ(cwd, expect);

    dsys_path("..");
    dir_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    dir_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    msc_expect(expect, sizeof(expect), "");
    ASSERT_STREQ(cwd, expect);

    dsys_path("..");
    dir_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    dir_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    ASSERT_STRNE(cwd, expect);
}

UTEST(fs, answers_in_the_host_s_spelling)
{
    ASSERT_TRUE(fresh_cwd());

    char cwd[TEST_PATH_MAX];
    dir_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    ASSERT_STREQ(cwd, g_dir);

    make_file("round.txt", "r", 1);
    char expect[TEST_PATH_MAX];
    msc_expect(expect, sizeof(expect), "/round.txt");
    ASSERT_EQ(strncmp(expect, host_drive(), strlen(host_drive())), 0);
    char *abs = os_dir_realpath("round.txt");
    ASSERT_TRUE(abs != NULL);
    ASSERT_STREQ(abs, expect);
    free(abs);
    abs = os_dir_realpath(expect);
    ASSERT_TRUE(abs != NULL);
    ASSERT_STREQ(abs, expect);
    free(abs);
}

/* U+65E5 U+672C, which no single-byte code page holds. */
static bool make_kanji_file(void)
{
#ifdef _WIN32
    FILE *f = _wfopen(L"\u65E5\u672C.txt", L"wb");
#else
    FILE *f = fopen("\xE6\x97\xA5\xE6\x9C\xAC.txt", "wb");
#endif
    if (f)
        fclose(f);
    return f != NULL;
}

UTEST(fs, a_name_the_code_page_cannot_hold_lists_with_127)
{
    ASSERT_TRUE(fresh_cwd());
    ASSERT_TRUE(make_kanji_file());
    make_file("plain.txt", "p", 1);
    int want = 2;
#ifndef _WIN32
    FILE *f = fopen("x:y.txt", "wb");
    ASSERT_TRUE(f != NULL);
    fclose(f);
    want++;
#endif

    dsys_path("");
    dir_api_opendir();
    int des = dsys_ax();
    ASSERT_TRUE(des >= 0);
    int count = 0;
    bool saw_kanji = false, saw_plain = false, saw_colon = false;
    f_stat_t info;
    for (;;)
    {
        dsys_des(des);
        dir_api_readdir();
        ASSERT_EQ(dsys_ax(), 0);
        dsys_filinfo(&info);
        if (!info.fname[0])
            break;
        count++;
        saw_kanji |= !strcmp(info.fname, "\x7F\x7F.txt");
        saw_plain |= !strcmp(info.fname, "plain.txt");
        saw_colon |= !strcmp(info.fname, "x\x7Fy.txt");
    }
    dsys_des(des);
    dir_api_closedir();
    ASSERT_EQ(count, want);
    ASSERT_TRUE(saw_kanji);
    ASSERT_TRUE(saw_plain);
#ifndef _WIN32
    ASSERT_TRUE(saw_colon);
#endif

    ASSERT_TRUE(ssys_open("\x7F\x7F.txt", O_RD) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EINVAL));
    dsys_path("\x7F\x7F.txt");
    dir_api_stat();
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EINVAL));
}

#ifndef _WIN32
/* Windows cannot make a folder this deep outside its long path form. */
UTEST(fs, a_working_directory_past_255_bytes_is_not_answered)
{
    ASSERT_TRUE(fresh_cwd());
    char seg[81];
    memset(seg, 'd', 80);
    seg[80] = 0;
    for (int i = 0; i < 3; i++)
    {
        ASSERT_TRUE(drive_mkdir_at(seg));
        ASSERT_TRUE(drive_chdir_to(seg));
    }
    dir_api_getcwd();
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENOMEM));
    ASSERT_TRUE(drive_chdir_to(g_dir));
}
#endif

UTEST(fs, chdrive_takes_this_machine_s_drive)
{
    ASSERT_TRUE(fresh_cwd());

    dsys_path(host_drive());
    dir_api_chdrive();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("");
    dir_api_chdrive();
    ASSERT_EQ(dsys_ax(), 0);

    dsys_path("NOPE:");
    dir_api_chdrive();
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENODEV));

    /* A drive name is not one without its colon. */
    char bare[8];
    snprintf(bare, sizeof bare, "%.*s", (int)strlen(host_drive()) - 1, host_drive());
    dsys_path(bare);
    dir_api_chdrive();
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENODEV));

    ASSERT_TRUE(drive_chdir_to(g_dir));
}

UTEST(fs, dir_enumeration)
{
    ASSERT_TRUE(fresh_cwd());
    make_file("alpha.txt", "hello", 5);
    make_file("beta.dat", "wider content here", 18);
    dsys_path("subdir");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);

    f_stat_t info;
    dsys_path("alpha.txt");
    dir_api_stat();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_filinfo(&info);
    ASSERT_EQ(info.fsize, 5u);
    ASSERT_TRUE(info.fattrib & AM_ARC);
    ASSERT_FALSE(info.fattrib & AM_DIR);
    ASSERT_STREQ(info.fname, "alpha.txt");
    dsys_path("subdir");
    dir_api_stat();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_filinfo(&info);
    ASSERT_TRUE(info.fattrib & AM_DIR);

    dsys_path("nope.txt");
    dir_api_stat();
    ASSERT_EQ(dsys_ax(), -1);

    dsys_path("");
    dir_api_opendir();
    int des = dsys_ax();
    ASSERT_TRUE(des >= 0);
    bool saw_alpha = false, saw_beta = false, saw_sub = false;
    int count = 0;
    for (;;)
    {
        dsys_des(des);
        dir_api_readdir();
        ASSERT_EQ(dsys_ax(), 0);
        dsys_filinfo(&info);
        if (!info.fname[0])
            break;
        ASSERT_STRNE(info.fname, ".");
        ASSERT_STRNE(info.fname, "..");
        if (!strcmp(info.fname, "alpha.txt"))
        {
            saw_alpha = true;
            ASSERT_EQ(info.fsize, 5u);
            ASSERT_TRUE(info.fattrib & AM_ARC);
        }
        else if (!strcmp(info.fname, "beta.dat"))
        {
            saw_beta = true;
            ASSERT_EQ(info.fsize, 18u);
        }
        else if (!strcmp(info.fname, "subdir"))
        {
            saw_sub = true;
            ASSERT_TRUE(info.fattrib & AM_DIR);
        }
        count++;
    }
    ASSERT_EQ(count, 3);
    ASSERT_TRUE(saw_alpha && saw_beta && saw_sub);

    dsys_des(des);
    dir_api_telldir();
    ASSERT_EQ(dsys_axsreg(), 3);
    dsys_des(des);
    dir_api_rewinddir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_des(des);
    dir_api_telldir();
    ASSERT_EQ(dsys_axsreg(), 0);
    dsys_des(des);
    dir_api_readdir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_filinfo(&info);
    ASSERT_TRUE(info.fname[0]);

    dsys_des(des);
    dir_api_closedir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_des(des);
    dir_api_readdir();
    ASSERT_EQ(dsys_ax(), -1);
    dsys_des(99);
    dir_api_readdir();
    ASSERT_EQ(dsys_ax(), -1);

    dsys_path("");
    while (dir_api_getfree())
        ;
    ASSERT_EQ(dsys_ax(), 0);
    uint32_t freeb = 0, totalb = 0;
    dsys_getfree(&freeb, &totalb);
    ASSERT_GT(totalb, 0u);
    ASSERT_TRUE(freeb <= totalb);

    /* Only the Pico has volume labels. */
    dsys_path("");
    dir_api_getlabel();
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EACCES));
    dsys_path("NEWLABEL");
    dir_api_setlabel();
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EACCES));

    dsys_chmod(AM_RDO, AM_RDO, "alpha.txt");
    dir_api_chmod();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("alpha.txt");
    dir_api_stat();
    dsys_filinfo(&info);
    ASSERT_TRUE(info.fattrib & AM_RDO);
    dsys_chmod(AM_RDO, 0, "alpha.txt");
    dir_api_chmod();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("alpha.txt");
    dir_api_stat();
    dsys_filinfo(&info);
    ASSERT_FALSE(info.fattrib & AM_RDO);

    /* FAT packs a date as (year - 1980) << 9 | month << 5 | day and a time as
     * hour << 11 | minute << 5 | seconds / 2, so this sets 08:00 on
     * 1990-03-15. */
    dsys_utime((8 << 11), (10 << 9) | (3 << 5) | 15, "beta.dat");
    dir_api_utime();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("beta.dat");
    dir_api_stat();
    dsys_filinfo(&info);
    ASSERT_EQ((unsigned)((info.fdate >> 9) & 0x7F), 10u);
    ASSERT_EQ((unsigned)((info.fdate >> 5) & 0x0F), 3u);
}

UTEST(fs, rom_asset_window_read_only_on_demand)
{
    ASSERT_TRUE(fresh_cwd());

    /* rom_load rejects a ROM without a reset vector, so the file has one
     * record for $FFFC and $FFFD. The first "#>" line gives the length of the
     * records after it, and the asset entries start where those records end. */
    unsigned char vec[2] = {0x00, 0x80};
    uint32_t vcrc = host_crc32(0, vec, 2);
    char rec[64];
    int recn = snprintf(rec, sizeof(rec), "$FFFC $2 $%X\r\n", vcrc);

    char rompath[300];
    snprintf(rompath, sizeof(rompath), "%s/asset.rp6502", host_dir());
    FILE *rf = fopen(rompath, "wb");
    ASSERT_TRUE(rf != NULL);
    fputs("#!RP6502\r\n", rf);
    fprintf(rf, "#>$%X $0\r\n", (unsigned)(recn + 2));
    fwrite(rec, 1, (size_t)recn, rf);
    fwrite(vec, 1, 2, rf);
    fputs("#>$3 $0 r.txt\r\n", rf);
    fwrite("abc", 1, 3, rf);
    fclose(rf);

    ASSERT_TRUE(rom_load(rompath));

    ASSERT_TRUE(ssys_open("ROM:r.txt", O_WR | O_CREAT_) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EACCES));

    int f = ssys_open("ROM:r.txt", O_RD);
    ASSERT_TRUE(f >= 0);
    ASSERT_TRUE(ssys_write(f, "x", 1) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENOSYS));
    char buf[8] = {0};
    ASSERT_EQ(ssys_read(f, buf, 8), 3);
    ASSERT_STREQ(buf, "abc");
    ASSERT_EQ(ssys_lseek(f, 1, SEEK_SET), 1);
    ASSERT_EQ(ssys_read(f, buf, 1), 1);
    ASSERT_EQ(buf[0], 'b');
    ssys_close(f);

    ASSERT_TRUE(ssys_open("ROM:missing.txt", O_RD) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENOENT));

    ASSERT_EQ(rom_read_asset("r.txt", buf, sizeof buf), 3);
    ASSERT_STREQ(buf, "abc");
    ASSERT_EQ(rom_read_asset("r.txt", buf, 3), 2);
    ASSERT_STREQ(buf, "ab");
    ASSERT_EQ(rom_read_asset("missing.txt", buf, sizeof buf), -1);
}

UTEST(fs, rom_asset_name_compares_through_the_code_page)
{
    ASSERT_TRUE(fresh_cwd());
    oem_set_code_page_run(437);

    unsigned char vec[2] = {0x00, 0x80};
    uint32_t vcrc = host_crc32(0, vec, 2);
    char rec[64];
    int recn = snprintf(rec, sizeof(rec), "$FFFC $2 $%X\r\n", vcrc);

    char rompath[300];
    snprintf(rompath, sizeof(rompath), "%s/cp.rp6502", host_dir());
    FILE *rf = fopen(rompath, "wb");
    ASSERT_TRUE(rf != NULL);
    fputs("#!RP6502\r\n", rf);
    fprintf(rf, "#>$%X $0\r\n", (unsigned)(recn + 2));
    fwrite(rec, 1, (size_t)recn, rf);
    fwrite(vec, 1, 2, rf);
    /* C3 A9 is é (U+00E9) in UTF-8, and CP437 has é at 0x82. */
    fputs("#>$2 $0 caf\xc3\xa9\r\n", rf);
    fwrite("ok", 1, 2, rf);
    fclose(rf);

    ASSERT_TRUE(rom_load(rompath));

    char oem_name[16] = {'R', 'O', 'M', ':', 'c', 'a', 'f', (char)0x82, 0};
    int f = ssys_open(oem_name, O_RD);
    ASSERT_TRUE(f >= 0);
    char buf[4] = {0};
    ASSERT_EQ(ssys_read(f, buf, 4), 2);
    ASSERT_STREQ(buf, "ok");
    ssys_close(f);
}

UTEST(fs, oem_names_roundtrip)
{
    str_init();
    ASSERT_TRUE(fresh_cwd());

    int f = ssys_open("nap\x82.txt", O_WR | O_CREAT_ | O_TRUNC_); /* CP437 'é' */
    ASSERT_TRUE(f >= 0);
    ASSERT_EQ(ssys_write(f, "x", 1), 1);
    ssys_close(f);

#ifndef _WIN32
    /* The POSIX file layer converts the CP437 name to UTF-8 before it calls
     * open, so the file on disk has the UTF-8 name. The Windows file layer
     * converts it to UTF-16 instead, so the check is left out there. */
    ASSERT_TRUE(host_exists("nap\xC3\xA9.txt"));
#endif

    f_stat_t info;
    dsys_path("");
    dir_api_opendir();
    int des = dsys_ax();
    ASSERT_TRUE(des >= 0);
    bool saw = false;
    for (;;)
    {
        dsys_des(des);
        dir_api_readdir();
        ASSERT_EQ(dsys_ax(), 0);
        dsys_filinfo(&info);
        if (!info.fname[0])
            break;
        if (!strcmp(info.fname, "nap\x82.txt"))
            saw = true;
    }
    dsys_des(des);
    dir_api_closedir();
    ASSERT_TRUE(saw);

    dsys_path("nap\x82.txt");
    dir_api_stat();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_filinfo(&info);
    ASSERT_STREQ(info.fname, "nap\x82.txt");

    dsys_path("nap\x82.txt");
    dir_api_unlink();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("nap\x82.txt");
    dir_api_stat();
    ASSERT_EQ(dsys_ax(), -1);
}

/* A zero-length read passes the driver a buffer at &xstack[XSTACK_SIZE],
 * which is the zero byte that terminates a string pushed to the top of the
 * xstack without a terminator. A driver that writes there leaves the next
 * such string unterminated. */
UTEST(fs, a_read_of_nothing_writes_nothing)
{
    ASSERT_TRUE(fresh_cwd());
    ASSERT_EQ(xstack[XSTACK_SIZE], 0);

    make_file("zero.txt", "hello", 5);
    int f = ssys_open("zero.txt", O_RD);
    ASSERT_TRUE(f >= 0);
    char buf[8] = {0};
    ASSERT_EQ(ssys_read(f, buf, 0), 0);
    ASSERT_EQ(xstack[XSTACK_SIZE], 0);
    ssys_close(f);

    /* A received byte is staged in the RX register at $FFE2 so that the
     * zero-length TTY read has a byte it could write. */
    std_init();
    regs[0x02] = 'X';
    regs[0x00] |= 0x40; /* RIA_UART_RX_READY */
    ASSERT_EQ(ssys_read(4 /* STD_FD_TTY */, buf, 0), 0);
    ASSERT_EQ(xstack[XSTACK_SIZE], 0);
    ASSERT_TRUE(regs[0x00] & 0x40);

    ASSERT_EQ(ssys_read(4, buf, 1), 1);
    ASSERT_EQ(buf[0], 'X');

    dsys_path("zero.txt");
    dir_api_stat();
    ASSERT_EQ(dsys_ax(), 0);
}

static int32_t stat_size(const char *path)
{
    dsys_path(path);
    dir_api_stat();
    if (dsys_ax() != 0)
        return -1;
    f_stat_t info;
    dsys_filinfo(&info);
    return (int32_t)info.fsize;
}

static void dsys_rename(const char *from, const char *to)
{
    size_t nf = strlen(from) + 1, nt = strlen(to) + 1;
    xstack_ptr = (uint16_t)(XSTACK_SIZE - nf - nt);
    memcpy(&xstack[xstack_ptr], to, nt);
    memcpy(&xstack[xstack_ptr + nt], from, nf);
}

UTEST(fs, a_save_lands_in_the_folder_fixed_at_start)
{
    ASSERT_TRUE(fresh_cwd());

    int f = ssys_open("SAVE:hopper.hiscore", O_WR | O_CREAT_ | O_TRUNC_);
    ASSERT_TRUE(f >= 0);
    ASSERT_EQ(ssys_write(f, "1234", 4), 4);
    ssys_close(f);
    ASSERT_TRUE(host_exists("hopper.hiscore"));

    dsys_path("sub");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("sub");
    dir_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);

    char buf[8] = {0};
    f = ssys_open("save:/hopper.hiscore", O_RD);
    ASSERT_TRUE(f >= 0);
    ASSERT_EQ(ssys_read(f, buf, 8), 4);
    ASSERT_EQ(memcmp(buf, "1234", 4), 0);
    ssys_close(f);
    ASSERT_TRUE(ssys_open("hopper.hiscore", O_RD) < 0);

    f = ssys_open("SAVE:\\hopper.hiscore", O_WR | O_APPEND_);
    ASSERT_TRUE(f >= 0);
    ASSERT_EQ(ssys_write(f, "5", 1), 1);
    ssys_close(f);
    ASSERT_EQ(stat_size("../hopper.hiscore"), 5);

    ASSERT_TRUE(ssys_open("SAVE:hopper.hiscore", O_WR | O_CREAT_ | O_EXCL_) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EEXIST));
}

UTEST(fs, a_save_name_follows_one_rule_everywhere)
{
    ASSERT_TRUE(fresh_cwd());

    static const char *const refused[] = {
        "SAVE:", "SAVE:/", "SAVE:..", "SAVE:../x", "SAVE:a/b", "SAVE:FS:/x",
        "SAVE:x.", "SAVE:a b", "SAVE:CON", "SAVE:con.txt", "SAVE:Com1",
        "SAVE:lpt9.sav", "SAVE:nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn"};
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++)
    {
        ASSERT_TRUE(ssys_open(refused[i], O_WR | O_CREAT_) < 0);
        ASSERT_EQ(ssys_errno(), api_platform_errno(API_EINVAL));
    }

    static const char *const taken[] = {
        "SAVE:fs_32_chars_long_name.0123456789", "SAVE:console.txt",
        "SAVE:hopper.X42"};
    for (size_t i = 0; i < sizeof taken / sizeof *taken; i++)
    {
        int f = ssys_open(taken[i], O_WR | O_CREAT_ | O_TRUNC_);
        ASSERT_TRUE(f >= 0);
        ssys_close(f);
    }
    ASSERT_TRUE(host_exists("fs_32_chars_long_name.0123456789"));
}

UTEST(fs, only_open_takes_save)
{
    ASSERT_TRUE(fresh_cwd());
    make_file("SAVE:kept.sav", "k", 1);
    ASSERT_TRUE(host_exists("kept.sav"));

    dsys_path("SAVE:x");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENODEV));
    dsys_path("SAVE:kept.sav");
    dir_api_stat();
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENODEV));
    dsys_path("SAVE:kept.sav");
    dir_api_unlink();
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENODEV));
    ASSERT_TRUE(host_exists("kept.sav"));
}

UTEST(fs, a_path_follows_the_fat_rules)
{
    ASSERT_TRUE(fresh_cwd());

    static const char *const no_drive[] = {"VCP0:x", "nope:x"};
    for (size_t i = 0; i < sizeof no_drive / sizeof *no_drive; i++)
    {
        ASSERT_TRUE(ssys_open(no_drive[i], O_WR | O_CREAT_) < 0);
        ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENODEV));
    }

    char drive_colon[16];
    snprintf(drive_colon, sizeof drive_colon, "%sx:y", host_drive());
    const char *const invalid[] = {
        "a/b:c", "/a:b", drive_colon, "a*b", "a?b", "a\"b", "a<b", "a>b",
        "a|b", "a\x01" "b", "a\x7F" "b"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
    {
        ASSERT_TRUE(ssys_open(invalid[i], O_WR | O_CREAT_) < 0);
        ASSERT_EQ(ssys_errno(), api_platform_errno(API_EINVAL));
    }
#ifndef _WIN32
    ASSERT_FALSE(host_exists("a*b"));
#endif

    dsys_path("sub");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);
    int f = ssys_open("sub\\f.txt", O_WR | O_CREAT_);
    ASSERT_TRUE(f >= 0);
    ssys_close(f);
    ASSERT_TRUE(host_exists("sub/f.txt"));

    ASSERT_TRUE(ssys_open("sub/f.txt", O_CREAT_) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EINVAL));

    char too_long[API_PATH_MAX + 2];
    memset(too_long, 'x', API_PATH_MAX + 1);
    too_long[API_PATH_MAX + 1] = 0;
    ASSERT_TRUE(ssys_open(too_long, O_RD) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EINVAL));
    dsys_path(too_long);
    dir_api_stat();
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EINVAL));
}

UTEST(fs, dotdot_at_a_root_stays_there)
{
    ASSERT_TRUE(fresh_cwd());
    char root[16];
    snprintf(root, sizeof root, "%s/", host_drive());
    dsys_path(root);
    dir_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    char cwd[TEST_PATH_MAX];
    dir_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    ASSERT_STREQ(cwd, root);
    dsys_path("..");
    dir_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    dir_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    ASSERT_STREQ(cwd, root);
    ASSERT_TRUE(drive_chdir_to(g_dir));
}

UTEST(fs, a_seek_past_the_end_fills_the_gap_with_zeros)
{
    ASSERT_TRUE(fresh_cwd());
    int f = ssys_open("gap.dat", O_WR | O_CREAT_ | O_TRUNC_);
    ASSERT_TRUE(f >= 0);
    ASSERT_EQ(ssys_write(f, "ab", 2), 2);
    ASSERT_EQ(ssys_lseek(f, 10, SEEK_SET), 10);
    ASSERT_EQ(ssys_lseek(f, 0, SEEK_END), 10);
    ssys_close(f);

    f = ssys_open("gap.dat", O_RD);
    ASSERT_TRUE(f >= 0);
    char buf[16];
    memset(buf, 0x55, sizeof buf);
    ASSERT_EQ(ssys_read(f, buf, 16), 10);
    ASSERT_EQ(memcmp(buf, "ab\0\0\0\0\0\0\0\0", 10), 0);
    ssys_close(f);
}

UTEST(fs, a_drive_root_has_one_fixed_entry)
{
    ASSERT_TRUE(fresh_cwd());
    char root[16];
    snprintf(root, sizeof root, "%s/", host_drive());
    const char *const roots[] = {"/", root};
    for (size_t i = 0; i < sizeof roots / sizeof *roots; i++)
    {
        dsys_path(roots[i]);
        dir_api_stat();
        ASSERT_EQ(dsys_ax(), 0);
        f_stat_t info;
        dsys_filinfo(&info);
        ASSERT_STREQ(info.fname, "/");
        ASSERT_EQ(info.fattrib, AM_DIR);
        ASSERT_EQ(info.fsize, 0u);
        ASSERT_EQ(info.fdate, 0);
        ASSERT_EQ(info.ftime, 0);
    }

    const char *const refused[] = {"", host_drive()};
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++)
    {
        dsys_path(refused[i]);
        dir_api_stat();
        ASSERT_EQ(dsys_ax(), -1);
        ASSERT_EQ(ssys_errno(), api_platform_errno(API_EINVAL));
    }
}

UTEST(fs, an_open_file_can_be_opened_again_renamed_and_unlinked)
{
    ASSERT_TRUE(fresh_cwd());
    int w = ssys_open("twice.txt", O_WR | O_CREAT_ | O_TRUNC_);
    ASSERT_TRUE(w >= 0);
    ASSERT_EQ(ssys_write(w, "abc", 3), 3);
    int r = ssys_open("twice.txt", O_RD);
    ASSERT_TRUE(r >= 0);

    dsys_rename("twice.txt", "moved.txt");
    dir_api_rename();
    ASSERT_EQ(dsys_ax(), 0);
    ASSERT_TRUE(host_exists("moved.txt"));
    dsys_path("moved.txt");
    dir_api_unlink();
    ASSERT_EQ(dsys_ax(), 0);
    ASSERT_FALSE(host_exists("moved.txt"));

    ASSERT_EQ(ssys_close(r), 0);
    ASSERT_EQ(ssys_close(w), 0);
}

UTEST(fs, an_open_of_a_directory_is_refused)
{
    ASSERT_TRUE(fresh_cwd());
    dsys_path("sub");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);
    ASSERT_TRUE(ssys_open("sub", O_RD) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EACCES));
    ASSERT_TRUE(ssys_open("sub", O_WR | O_CREAT_) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EACCES));
}

UTEST(fs, a_rename_replaces_only_a_file_with_a_file)
{
    ASSERT_TRUE(fresh_cwd());
    make_file("a.txt", "A", 1);
    make_file("b.txt", "BB", 2);
    dsys_path("d1");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("d2");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);

    dsys_rename("a.txt", "b.txt");
    dir_api_rename();
    ASSERT_EQ(dsys_ax(), 0);
    ASSERT_FALSE(host_exists("a.txt"));
    ASSERT_EQ(stat_size("b.txt"), 1);

    static const char *const taken[][2] = {
        {"b.txt", "d1"}, {"d1", "d2"}, {"d1", "b.txt"}};
    for (size_t i = 0; i < sizeof taken / sizeof *taken; i++)
    {
        dsys_rename(taken[i][0], taken[i][1]);
        dir_api_rename();
        ASSERT_EQ(dsys_ax(), -1);
        ASSERT_EQ(ssys_errno(), api_platform_errno(API_EEXIST));
    }
    ASSERT_EQ(stat_size("b.txt"), 1);

    dsys_rename("b.txt", "NOPE:b.txt");
    dir_api_rename();
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENODEV));
    ASSERT_TRUE(host_exists("b.txt"));
}

UTEST(fs, an_unlink_of_a_read_only_file_is_refused)
{
    ASSERT_TRUE(fresh_cwd());
    make_file("ro.txt", "R", 1);
    dsys_chmod(AM_RDO, AM_RDO, "ro.txt");
    dir_api_chmod();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("ro.txt");
    dir_api_unlink();
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EACCES));
    ASSERT_TRUE(host_exists("ro.txt"));

    dsys_chmod(AM_RDO, 0, "ro.txt");
    dir_api_chmod();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("ro.txt");
    dir_api_unlink();
    ASSERT_EQ(dsys_ax(), 0);
    ASSERT_FALSE(host_exists("ro.txt"));
}

#ifdef _WIN32
UTEST(fs, a_name_windows_keeps_for_a_device_is_refused)
{
    ASSERT_TRUE(fresh_cwd());
    dsys_path("sub");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);
    static const char *const reserved[] = {
        "NUL", "con", "Aux", "PRN", "com1", "LPT9", "CONIN$", "conout$",
        "sub/nul"};
    for (size_t i = 0; i < sizeof reserved / sizeof *reserved; i++)
    {
        ASSERT_TRUE(ssys_open(reserved[i], O_WR | O_CREAT_) < 0);
        ASSERT_EQ(ssys_errno(), api_platform_errno(API_EINVAL));
    }
    dsys_path("PRN");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EINVAL));
}
#endif

static uint8_t g_blob[STD_SST_SIZE];
static int g_save_fd, g_plain_fd;

/* A save and a plain file, each open with its position partway in, are
 * recorded in a savestate with their flags. SAVE: then moves to a new folder
 * whose save of the same name holds other bytes, so only a reopen by name
 * reads those. */
static bool take_two_files(unsigned flags, sst_cursor_t *c)
{
    if (!fresh_cwd())
        return false;
    make_file("SAVE:slot.sav", "ABCDEFGH", 8);
    make_file("plain.txt", "pqrstuvw", 8);
    g_save_fd = ssys_open("SAVE:slot.sav", O_RD);
    g_plain_fd = ssys_open("plain.txt", O_RD);
    char buf[4];
    if (g_save_fd < 0 || g_plain_fd < 0 ||
        ssys_read(g_save_fd, buf, 2) != 2 || ssys_read(g_plain_fd, buf, 3) != 3)
        return false;
    *c = (sst_cursor_t){g_blob, g_blob + sizeof g_blob, false};
    std_sst_save(c, flags);
    if (!sst_ok(c) || !fresh_cwd())
        return false;
    make_file("SAVE:slot.sav", "abcdefgh", 8);
    return true;
}

UTEST(fs, a_savestate_reopens_a_save_by_its_name)
{
    sst_cursor_t c;
    ASSERT_TRUE(take_two_files(0, &c));
    sst_cursor_t r = {g_blob, c.at, false};
    ASSERT_TRUE(std_sst_load(&r, 0));

    char buf[4] = {0};
    ASSERT_EQ(ssys_read(g_save_fd, buf, 2), 2);
    ASSERT_EQ(memcmp(buf, "cd", 2), 0);
    ASSERT_EQ(ssys_read(g_plain_fd, buf, 1), 1);
    ASSERT_EQ(buf[0], 's');
}

UTEST(fs, a_shared_savestate_carries_saves_and_closes_host_files)
{
    sst_cursor_t c;
    ASSERT_TRUE(take_two_files(SST_SHARED, &c));
    sst_cursor_t r = {g_blob, c.at, false};
    ASSERT_TRUE(std_sst_load(&r, SST_SHARED));

    char buf[4] = {0};
    ASSERT_EQ(ssys_read(g_save_fd, buf, 2), 2);
    ASSERT_EQ(memcmp(buf, "cd", 2), 0);
    ASSERT_TRUE(ssys_read(g_plain_fd, buf, 1) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EBADF));
}

UTEST_MAIN()
