/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/dir.h"
#include "core/api/std.h"
#include "core/rom/rom.h"
#include "osal/fs.h"
#include "osal/os.h"
#include "core/ria/regs.h"
#include "core/sys/xram.h"
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
    if (!drive_chdir_to(dir))
        return false;
    return drive_cwd(g_dir, sizeof(g_dir));
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


UTEST(drive, rom_resolve_and_load)
{
    ASSERT_TRUE(fresh());

    make_file("adventure.rp6502", "NOT THE ROM", 11);

    ASSERT_TRUE(rom_alias_insert(TEST_FIXTURE));

    make_file("second.rp6502", "#!RP6502 two", 12);
    char second[TEST_PATH_MAX];
    snprintf(second, sizeof(second), "%s/second.rp6502", g_dir);
    ASSERT_TRUE(rom_alias_insert(second));

    ASSERT_STREQ(rom_alias_resolve(":adventure.rp6502"), TEST_FIXTURE);
    ASSERT_STREQ(rom_alias_resolve(":ADVENTURE.RP6502"), TEST_FIXTURE);
    ASSERT_STREQ(rom_alias_resolve(":second.rp6502"), second);
    const char *unaliased = ":nope.rp6502";
    ASSERT_EQ(rom_alias_resolve(unaliased), unaliased);
    ASSERT_FALSE(rom_load(":nope.rp6502"));

    ASSERT_TRUE(rom_load(":adventure.rp6502"));

    ASSERT_TRUE(ssys_open(":adventure.rp6502", O_RD) < 0);
    ASSERT_TRUE(ssys_open(":", O_RD) < 0);

    int f = ssys_open("adventure.rp6502", O_RD);
    ASSERT_TRUE(f >= 0);
    char buf[8] = {0};
    ASSERT_EQ(ssys_read(f, buf, 8), 8);
    ASSERT_EQ(memcmp(buf, "NOT THE ", 8), 0);
    ssys_close(f);
}

UTEST(drive, fs_rom_open_is_the_one_way_in)
{
    ASSERT_TRUE(fresh());
    ASSERT_TRUE(rom_alias_insert(TEST_FIXTURE));

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
    ASSERT_TRUE(rom_alias_insert(TEST_FIXTURE));

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
    snprintf(hostprobe, sizeof(hostprobe), "%s/save.dat", g_dir);
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

UTEST_MAIN()
