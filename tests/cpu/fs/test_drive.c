/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/arg.h"
#include "core/api/dir.h"
#include "core/api/proc.h"
#include "core/api/std.h"
#include "core/rom/rom.h"
#include "core/sys/proc.h"
#include "core/sys/sys.h"
#include "osal/fs.h"
#include "osal/os.h"
#include "core/ria/regs.h"
#include "core/sys/xram.h"
#include "dirsys.h"
#include "emu_boot.h"
#include "stdsys.h"
#include "tb_hostos.h"
#include "utest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#define O_RD 0x01
#define O_WR 0x02
#define O_CREAT_ 0x10
#define O_TRUNC_ 0x20

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

static bool fresh(void)
{
    char dir[TEST_PATH_MAX];
    if (!host_make_tmpdir(dir, sizeof(dir)))
        return false;
    std_stop();
    if (!drive_chdir_to(dir) || !drive_cwd(g_dir, sizeof(g_dir)))
        return false;
    api_set_errno_opt(2);
    return true;
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


static void msc_expect(char *out, size_t sz, const char *suffix)
{
    snprintf(out, sz, "%s%s", g_dir, suffix);
}

/* g_dir as a host path, without the FS: that the POSIX GETCWD puts in
 * front. */
static const char *host_dir(void)
{
    return strncmp(g_dir, "FS:", 3) ? g_dir : g_dir + 3;
}

/* The absolute form of a host path, for the caller to free. */
static char *host_abs(const char *host)
{
#ifdef _WIN32
    return fs_host_realpath(host);
#else
    return realpath(host, NULL);
#endif
}

static bool copy_fixture(const char *host)
{
    FILE *in = fopen(TEST_FIXTURE, "rb");
    FILE *out = fopen(host, "wb");
    bool ok = in && out;
    char buf[4096];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0)
        ok = fwrite(buf, 1, n, out) == n;
    if (in)
        fclose(in);
    if (out && fclose(out) != 0)
        ok = false;
    return ok;
}

/* The argv a program passes to EXEC: one offset, the zero pair that ends the
 * table, then argv[0]. */
static void push_exec(const char *argv0)
{
    size_t n = strlen(argv0) + 1;
    xstack_ptr = (uint16_t)(XSTACK_SIZE - 4 - n);
    uint8_t *p = &xstack[xstack_ptr];
    p[0] = 4, p[1] = 0, p[2] = 0, p[3] = 0;
    memcpy(p + 4, argv0, n);
}


UTEST(drive, rom_resolve_and_load)
{
    ASSERT_TRUE(fresh());

    make_file("adventure.rp6502", "NOT THE ROM", 11);

    ASSERT_STREQ(rom_alias_insert(TEST_FIXTURE), "adventure.rp6502");

    make_file("second.rp6502", "#!RP6502 two", 12);
    ASSERT_STREQ(rom_alias_insert("second.rp6502"), "second.rp6502");

    char *fixture = host_abs(TEST_FIXTURE);
    char *second = host_abs("second.rp6502");
    ASSERT_TRUE(fixture != NULL);
    ASSERT_TRUE(second != NULL);

    /* An install opens the file it was given from any working directory. */
    dsys_path("elsewhere");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("elsewhere");
    dir_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    ASSERT_STREQ(rom_alias_resolve(":adventure.rp6502"), fixture);
    ASSERT_STREQ(rom_alias_resolve(":ADVENTURE.RP6502"), fixture);
    ASSERT_STREQ(rom_alias_resolve(":second.rp6502"), second);
    free(fixture);
    free(second);
    ASSERT_TRUE(rom_alias_resolve(":nope.rp6502") == NULL);
    ASSERT_TRUE(rom_alias_resolve("adventure.rp6502") == NULL);
    ASSERT_FALSE(rom_load(":nope.rp6502"));

    ASSERT_TRUE(rom_load(":adventure.rp6502"));

    ASSERT_TRUE(ssys_open(":adventure.rp6502", O_RD) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENODEV));
    ASSERT_TRUE(ssys_open(":", O_RD) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENODEV));

    dsys_path("..");
    dir_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    int f = ssys_open("adventure.rp6502", O_RD);
    ASSERT_TRUE(f >= 0);
    char buf[8] = {0};
    ASSERT_EQ(ssys_read(f, buf, 8), 8);
    ASSERT_EQ(memcmp(buf, "NOT THE ", 8), 0);
    ssys_close(f);
}

UTEST(drive, an_install_replaces_one_of_the_same_name)
{
    ASSERT_TRUE(fresh());
    make_file("adventure.rp6502", "NOT THE ROM", 11);
    char *own = host_abs("adventure.rp6502");
    char *fixture = host_abs(TEST_FIXTURE);
    ASSERT_TRUE(own != NULL);
    ASSERT_TRUE(fixture != NULL);

    ASSERT_STREQ(rom_alias_insert("adventure.rp6502"), "adventure.rp6502");
    ASSERT_STREQ(rom_alias_resolve(":adventure.rp6502"), own);
    ASSERT_STREQ(rom_alias_insert_as(TEST_FIXTURE, "ADVENTURE.RP6502"),
                 "ADVENTURE.RP6502");
    ASSERT_STREQ(rom_alias_resolve(":adventure.rp6502"), fixture);
    free(own);
    free(fixture);

    ASSERT_TRUE(rom_alias_remove("adventure.rp6502"));
    ASSERT_TRUE(rom_alias_resolve(":adventure.rp6502") == NULL);
    ASSERT_TRUE(rom_alias_insert(TEST_FIXTURE) != NULL);
}

#ifndef _WIN32
/* The folder is named U+65E5 U+672C, which no single-byte code page holds, so
 * only an install can run the ROM in it. */
UTEST(drive, an_install_keeps_a_host_path_the_code_page_cannot_hold)
{
    ASSERT_TRUE(fresh());
    char dir[TEST_PATH_MAX + 16], rom[TEST_PATH_MAX + 32];
    snprintf(dir, sizeof dir, "%s/\xE6\x97\xA5\xE6\x9C\xAC", host_dir());
    ASSERT_EQ(mkdir(dir, 0700), 0);
    snprintf(rom, sizeof rom, "%s/z.rp6502", dir);
    ASSERT_TRUE(copy_fixture(rom));

    ASSERT_STREQ(rom_alias_insert(rom), "z.rp6502");
    ASSERT_STREQ(rom_alias_resolve(":z.rp6502"), rom);
    ASSERT_TRUE(rom_load(":z.rp6502"));
    ASSERT_TRUE(rom_alias_remove(":z.rp6502"));
}
#endif

UTEST(drive, fs_rom_open_is_the_one_way_in)
{
    ASSERT_TRUE(fresh());
    ASSERT_TRUE(rom_alias_insert(TEST_FIXTURE) != NULL);

    api_errno err;
    int fd = fs_rom_open(TEST_FIXTURE, FS_RD, &err);
    ASSERT_TRUE(fd >= 0);
    char magic[8] = {0};
    uint32_t got = 0;
    std_rw_result r;
    do
        r = fs_std_read(fd, magic, 8, &got, &err);
    while (r == STD_PENDING);
    ASSERT_EQ(r, STD_OK);
    ASSERT_EQ(memcmp(magic, "#!RP6502", 8), 0);

    ASSERT_TRUE(ssys_read(fd, magic, 1) < 0);
    ASSERT_TRUE(ssys_close(fd) < 0);

    fs_std_close(fd, &err);

    ASSERT_TRUE(fs_rom_open(":adventure.rp6502", FS_RD, &err) < 0);

    ASSERT_TRUE(fs_rom_open(":new.rp6502", FS_WR | FS_CREAT | FS_EXCL, &err) < 0);
    ASSERT_EQ(err, API_EACCES);
    ASSERT_TRUE(fs_rom_open(TEST_FIXTURE, FS_WR, &err) < 0);
    ASSERT_EQ(err, API_EINVAL);
    ASSERT_FALSE(fs_rom_remove(":adventure.rp6502", &err));
    ASSERT_EQ(err, API_EACCES);
}


UTEST(drive, install_null_drive_has_no_cwd_dir_stat)
{
    ASSERT_TRUE(fresh());
    ASSERT_TRUE(rom_alias_insert(TEST_FIXTURE) != NULL);

    dsys_path(":adventure.rp6502");
    dir_api_stat();
    ASSERT_EQ(dsys_ax(), -1);
    dsys_path(":");
    dir_api_opendir();
    ASSERT_EQ(dsys_ax(), -1);
    dsys_path(":adventure.rp6502");
    dir_api_chdir();
    ASSERT_EQ(dsys_ax(), -1);
    dsys_path(":");
    dir_api_chdrive();
    ASSERT_EQ(dsys_ax(), -1);
    dsys_path(":adventure.rp6502");
    dir_api_unlink();
    ASSERT_EQ(dsys_ax(), -1);
    dsys_path(":sub");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), -1);
    char aliased[64];
    snprintf(aliased, sizeof(aliased), "%s:adventure.rp6502", host_drive());
    dsys_path(aliased);
    dir_api_stat();
    ASSERT_EQ(dsys_ax(), -1);
}

UTEST(drive, mount_transparent_no_chroot)
{
    ASSERT_TRUE(fresh());

    char cwd[TEST_PATH_MAX], expect[TEST_PATH_MAX];
    dir_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    msc_expect(expect, sizeof(expect), "");
    ASSERT_STREQ(cwd, expect);

    char named[64];
    snprintf(named, sizeof(named), "%ssave.dat", host_drive());
    int f = ssys_open(named, O_WR | O_CREAT_ | O_TRUNC_);
    ASSERT_TRUE(f >= 0);
    ssys_close(f);
    char hostprobe[512];
    snprintf(hostprobe, sizeof(hostprobe), "%s/save.dat", host_dir());
    FILE *hp = fopen(hostprobe, "rb");
    ASSERT_TRUE(hp != NULL);
    if (hp)
        fclose(hp);

    dsys_path("sub");
    dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("sub");
    dir_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
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

UTEST(drive, a_boot_by_relative_path_leaves_argv0_absolute)
{
    ASSERT_TRUE(fresh());
    char rom[TEST_PATH_MAX + 16];
    snprintf(rom, sizeof rom, "%s/x.rp6502", host_dir());
    ASSERT_TRUE(copy_fixture(rom));

    ASSERT_TRUE(emu_restart("x.rp6502"));
    char expect[TEST_PATH_MAX];
    msc_expect(expect, sizeof(expect), "/x.rp6502");
    ASSERT_STREQ(arg_index(0), expect);
    ASSERT_EQ(strncmp(arg_index(0), host_drive(), strlen(host_drive())), 0);

    sys_stop();
    sys_commit();
}

UTEST(drive, an_exec_by_relative_path_leaves_argv0_absolute)
{
    ASSERT_TRUE(fresh());
    char rom[TEST_PATH_MAX + 16];
    snprintf(rom, sizeof rom, "%s/y.rp6502", host_dir());
    ASSERT_TRUE(copy_fixture(rom));
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    api_set_errno_opt(2);

    char too_long[API_PATH_MAX + 2];
    memset(too_long, 'y', API_PATH_MAX + 1);
    too_long[API_PATH_MAX + 1] = 0;
    push_exec(too_long);
    ASSERT_FALSE(proc_api_exec());
    ASSERT_EQ(dsys_ax(), -1);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EINVAL));
    ASSERT_FALSE(proc_exec_inflight());

    push_exec("y.rp6502");
    ASSERT_FALSE(proc_api_exec());
    ASSERT_EQ(dsys_ax(), 0);
    ASSERT_TRUE(proc_exec_inflight());
    sys_commit();
    proc_exec_task();
    sys_commit();
    ASSERT_TRUE(sys_running());

    char expect[TEST_PATH_MAX];
    msc_expect(expect, sizeof(expect), "/y.rp6502");
    ASSERT_STREQ(arg_index(0), expect);
    ASSERT_EQ(strncmp(arg_index(0), host_drive(), strlen(host_drive())), 0);

    sys_stop();
    sys_commit();
}

/* A 3000-byte read is larger than the 2048-byte chunk that std_api_read_xram
 * reads at a time, so the first read of the 5000-byte file takes two chunks. */
static void async_aio_body(int *utest_result)
{
    char src[5000];
    for (size_t i = 0; i < sizeof(src); i++)
        src[i] = (char)(i * 7 + 1);

    memcpy((uint8_t *)&xram[0x1000], src, sizeof(src));
    int fd = ssys_open("async.dat", O_WR | O_CREAT_ | O_TRUNC_);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ssys_write_xram(fd, 0x1000, sizeof(src)), (int)sizeof(src));
    ssys_close(fd);

    fd = ssys_open("async.dat", O_RD);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ssys_read_xram(fd, 0x8000, 3000), 3000);
    ASSERT_EQ(memcmp((const uint8_t *)&xram[0x8000], src, 3000), 0);
    ASSERT_EQ(ssys_read_xram(fd, 0x8000, 3000), 2000);
    ASSERT_EQ(memcmp((const uint8_t *)&xram[0x8000], src + 3000, 2000), 0);
    ASSERT_EQ(ssys_read_xram(fd, 0x8000, 1000), 0);
    ASSERT_EQ(ssys_lseek(fd, 500, SEEK_SET), 500);
    char buf[16];
    ASSERT_EQ(ssys_read(fd, buf, 16), 16);
    ASSERT_EQ(memcmp(buf, src + 500, 16), 0);
    ssys_close(fd);
}

UTEST(drive, async_aio_transfer)
{
    ASSERT_TRUE(fresh());
    async_aio_body(utest_result);
}

UTEST_MAIN_EMU()
