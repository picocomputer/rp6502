/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/dir.h"
#include "core/api/std.h"
#include "core/rom/rom.h"
#include "osal/fs.h"
#include "core/sys/xram.h"
#include "emu_boot.h"
#include "stdsys.h"
#include "tb_hostos.h"
#include <stdio.h>
#include <string.h>

#define O_RD 0x01
#define O_WR 0x02
#define O_CREAT_ 0x10
#define O_TRUNC_ 0x20

static bool drive_chdir_to(const char *path)
{
    api_errno err;
    return drive_chdir(path, &err);
}

static bool fresh(void)
{
    char dir[TEST_PATH_MAX];
    if (!host_make_tmpdir(dir, sizeof(dir)))
        return false;
    std_stop();
    if (!drive_chdir_to(dir))
        return false;
    api_set_errno_opt(2);
    return true;
}

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


/* A 3000-byte read is larger than the 2048-byte chunk that std_api_read_xram
 * reads at a time, so the first read of the 5000-byte file takes two chunks.
 * A script run finishes file I/O at once, so only this test reaches a read
 * that is still pending. */
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
