/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for the host-backed filesystem: the MSC0: drive IS the native host
 * filesystem (no chroot — MSC0:/ is the OS root, a relative path resolves the
 * process cwd, ".." walks freely), plus the read-only ROM: drive. Drives the
 * backend API directly — the std_* file calls and the host dir/path helpers the
 * 6502's std.c/dir.c reach from the syscalls.
 */

#include "emu/api/api.h"
#include "emu/api/std.h"
#include "emu/mon/rom.h"
#include "emu/host/dir.h"
#include "utest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* rp6502 SDK open() flag bits (see ria/usb/msc.c). */
#define O_RD 0x01
#define O_WR 0x02
#define O_CREAT_ 0x10
#define O_TRUNC_ 0x20

static char g_dir[256]; /* a temp dir, made the MSC0: cwd */

static bool fresh_cwd(void)
{
    char tmpl[] = "/tmp/msc0_test_XXXXXX";
    const char *d = mkdtemp(tmpl);
    char resolved[FS_HOST_MAX_PATH]; /* realpath needs a PATH_MAX buffer */
    if (!d || !realpath(d, resolved) || strlen(resolved) >= sizeof(g_dir))
        return false;
    strcpy(g_dir, resolved);
    std_files_reset(); /* close any files a prior test left open */
    return chdir(g_dir) == 0; /* MSC0: is the process cwd */
}

static bool host_exists(const char *rel)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", g_dir, rel);
    FILE *f = fopen(p, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

/* MSC0: paths are host paths: a relative name resolves under the cwd; an
 * absolute "MSC0:<hostpath>" reaches the real filesystem. */
UTEST(fs, msc0_write_read_seek)
{
    ASSERT_TRUE(fresh_cwd());
    api_errno err;

    uint32_t got = 0, put = 0;
    int f = std_open("hello.txt", O_WR | O_CREAT_ | O_TRUNC_, NULL);
    ASSERT_TRUE(f >= 0);
    ASSERT_TRUE(std_writable(f));
    ASSERT_EQ(std_write(f, "hello", 5, &put, &err), STD_OK);
    ASSERT_EQ(put, 5u);
    std_close(f);
    ASSERT_TRUE(host_exists("hello.txt")); /* a real file under the cwd */

    f = std_open("hello.txt", O_RD, NULL);
    ASSERT_TRUE(f >= 0);
    char buf[8] = {0};
    ASSERT_EQ(std_read(f, buf, 8, &got, &err), STD_OK);
    ASSERT_EQ(got, 5u);
    ASSERT_STREQ(buf, "hello");
    ASSERT_EQ(std_lseek(f, 1, SEEK_SET), 1L);
    ASSERT_EQ(std_read(f, buf, 1, &got, &err), STD_OK);
    ASSERT_EQ(got, 1u);
    ASSERT_EQ(buf[0], 'e');
    std_close(f);

    /* The same file by its MSC0: (relative) path -> the process cwd. */
    f = std_open("MSC0:hello.txt", O_RD, NULL);
    ASSERT_TRUE(f >= 0);
    std_close(f);

    ASSERT_EQ(host_unlink("hello.txt", &err), 0);
    ASSERT_FALSE(host_exists("hello.txt"));
}

UTEST(fs, chdir_getcwd_relative)
{
    ASSERT_TRUE(fresh_cwd());
    api_errno err;

    char cwd[FS_HOST_MAX_PATH], expect[FS_HOST_MAX_PATH];
    host_getcwd(cwd, sizeof(cwd), &err);
    snprintf(expect, sizeof(expect), "MSC0:%s", g_dir); /* getcwd is the native cwd */
    ASSERT_STREQ(cwd, expect);

    ASSERT_EQ(host_mkdir("saves", &err), 0);
    ASSERT_EQ(host_chdir("saves", &err), 0);
    host_getcwd(cwd, sizeof(cwd), &err);
    snprintf(expect, sizeof(expect), "MSC0:%s/saves", g_dir);
    ASSERT_STREQ(cwd, expect);

    /* A too-small buffer fails (full-path-or-error), so the caller never
     * relocates a truncated path. */
    char tiny[8];
    ASSERT_EQ(host_getcwd(tiny, sizeof(tiny), &err), -1);

    /* A relative path resolves under the new cwd. */
    int f = std_open("game.sav", O_WR | O_CREAT_ | O_TRUNC_, NULL);
    ASSERT_TRUE(f >= 0);
    std_close(f);
    ASSERT_TRUE(host_exists("saves/game.sav"));

    ASSERT_EQ(host_chdir("nope", &err), -1); /* missing dir fails */
}

/* No chroot: MSC0: is the whole native filesystem, so ".." walks freely up the
 * real tree (the old mount-root confinement is gone). */
UTEST(fs, no_chroot_clamp)
{
    ASSERT_TRUE(fresh_cwd());
    api_errno err;

    ASSERT_EQ(host_mkdir("sub", &err), 0);
    ASSERT_EQ(host_chdir("sub", &err), 0);
    char cwd[FS_HOST_MAX_PATH], expect[FS_HOST_MAX_PATH];
    host_getcwd(cwd, sizeof(cwd), &err);
    snprintf(expect, sizeof(expect), "MSC0:%s/sub", g_dir);
    ASSERT_STREQ(cwd, expect);

    /* ".." climbs back to the launch dir ... */
    ASSERT_EQ(host_chdir("..", &err), 0);
    host_getcwd(cwd, sizeof(cwd), &err);
    snprintf(expect, sizeof(expect), "MSC0:%s", g_dir);
    ASSERT_STREQ(cwd, expect);

    /* ... and again climbs ABOVE it — no clamp; the cwd walks the real tree. */
    ASSERT_EQ(host_chdir("..", &err), 0);
    host_getcwd(cwd, sizeof(cwd), &err);
    ASSERT_STRNE(cwd, expect);
}

/* The MSC0:<->host translation: MSC0: maps straight onto the native filesystem
 * (absolute from the OS root); the Windows //C/ form names an explicit drive. */
UTEST(fs, path_translation)
{
    char host[FS_HOST_MAX_PATH], msc[FS_HOST_MAX_PATH];

    ASSERT_TRUE(fs_to_host("MSC0:/sub/file", host, sizeof(host)));
    ASSERT_STREQ(host, "/sub/file");
    ASSERT_TRUE(fs_to_host("0:/sub/file", host, sizeof(host))); /* numeric drive alias */
    ASSERT_STREQ(host, "/sub/file");
    ASSERT_TRUE(fs_to_host("MSC0://C/Users/Homey", host, sizeof(host)));
    ASSERT_STREQ(host, "C:/Users/Homey");

    fs_host_to_msc("/sub/file", msc, sizeof(msc));
    ASSERT_STREQ(msc, "MSC0:/sub/file");
    fs_host_to_msc("C:/Users/Homey", msc, sizeof(msc));
    ASSERT_STREQ(msc, "MSC0://C/Users/Homey"); /* another drive keeps //C/ */
}

UTEST(fs, chdrive_stays_on_msc0)
{
    ASSERT_TRUE(fresh_cwd());
    api_errno err;
    ASSERT_EQ(host_chdrive("MSC0:", &err), 0);
    ASSERT_EQ(host_chdrive("MSC0", &err), 0);
    ASSERT_EQ(host_chdrive("Z:", &err), -1);
    ASSERT_EQ(err, API_ENODEV);
}

/* FatFs attribute bits the 6502 sees (FatFs AM_*). */
#define AM_RDO 0x01
#define AM_DIR 0x10
#define AM_ARC 0x20

static void make_file(const char *rel, const char *data, uint32_t n)
{
    int f = std_open(rel, O_WR | O_CREAT_ | O_TRUNC_, NULL);
    if (f >= 0)
    {
        uint32_t put;
        api_errno err;
        std_write(f, data, n, &put, &err);
        std_close(f);
    }
}

/* Directory enumeration + stat + free space against a real temp directory. */
UTEST(fs, dir_enumeration)
{
    ASSERT_TRUE(fresh_cwd());
    api_errno err;
    make_file("alpha.txt", "hello", 5);
    make_file("beta.dat", "wider content here", 18);
    ASSERT_EQ(host_mkdir("subdir", &err), 0);

    /* stat reports size + synthesized FAT attributes. */
    FILINFO info;
    ASSERT_EQ(host_stat("alpha.txt", &info, &err), 0);
    ASSERT_EQ(info.fsize, 5u);
    ASSERT_TRUE(info.fattrib & AM_ARC);
    ASSERT_FALSE(info.fattrib & AM_DIR);
    ASSERT_STREQ(info.fname, "alpha.txt");
    ASSERT_EQ(host_stat("subdir", &info, &err), 0);
    ASSERT_TRUE(info.fattrib & AM_DIR);

    ASSERT_EQ(host_stat("nope.txt", &info, &err), -1); /* ENOENT surfaces */
    ASSERT_EQ(err, API_ENOENT);

    /* opendir/readdir lists exactly the three entries; "." and ".." are
     * skipped like FatFs; entry order is filesystem-defined, so match by name. */
    int des = host_opendir("", &err);
    ASSERT_TRUE(des >= 0);
    bool saw_alpha = false, saw_beta = false, saw_sub = false;
    int count = 0;
    for (;;)
    {
        ASSERT_EQ(host_readdir(des, &info, &err), 0);
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

    /* telldir tracks the entry index; rewinddir restarts it. */
    int32_t pos;
    ASSERT_EQ(host_telldir(des, &pos, &err), 0);
    ASSERT_EQ(pos, 3);
    ASSERT_EQ(host_rewinddir(des, &err), 0);
    ASSERT_EQ(host_telldir(des, &pos, &err), 0);
    ASSERT_EQ(pos, 0);
    ASSERT_EQ(host_readdir(des, &info, &err), 0);
    ASSERT_TRUE(info.fname[0]); /* an entry again after rewind */

    ASSERT_EQ(host_closedir(des, &err), 0);
    ASSERT_EQ(host_readdir(des, &info, &err), -1); /* closed handle -> EBADF */
    ASSERT_EQ(err, API_EBADF);
    ASSERT_EQ(host_readdir(99, &info, &err), -1); /* out-of-range -> EINVAL */
    ASSERT_EQ(err, API_EINVAL);

    /* getfree reports real host free/total space (in 512-byte sectors). */
    uint32_t freeb = 0, totalb = 0;
    ASSERT_EQ(host_getfree("", &freeb, &totalb, &err), 0);
    ASSERT_GT(totalb, 0u);
    ASSERT_TRUE(freeb <= totalb);

    /* no host volume label: empty string, success. */
    char label[16] = "x";
    ASSERT_EQ(host_getlabel("", label, &err), 0);
    ASSERT_STREQ(label, "");

    /* chmod toggles the read-only bit (the one FAT attribute with a host
     * equivalent), visible back through stat. */
    ASSERT_EQ(host_chmod("alpha.txt", AM_RDO, AM_RDO, &err), 0);
    ASSERT_EQ(host_stat("alpha.txt", &info, &err), 0);
    ASSERT_TRUE(info.fattrib & AM_RDO);
    ASSERT_EQ(host_chmod("alpha.txt", 0, AM_RDO, &err), 0);
    ASSERT_EQ(host_stat("alpha.txt", &info, &err), 0);
    ASSERT_FALSE(info.fattrib & AM_RDO);

    /* utime sets the modification date (FAT-packed: 1990-03-15). */
    FILINFO ut = {0};
    ut.fdate = (10 << 9) | (3 << 5) | 15;
    ut.ftime = (8 << 11);
    ASSERT_EQ(host_utime("beta.dat", &ut, &err), 0);
    ASSERT_EQ(host_stat("beta.dat", &info, &err), 0);
    ASSERT_EQ((unsigned)((info.fdate >> 9) & 0x7F), 10u); /* 1980 + 10 = 1990 */
    ASSERT_EQ((unsigned)((info.fdate >> 5) & 0x0F), 3u);  /* March */
}

/* A ROM: asset is a read-only WINDOW into the backing .rp6502 (no bytes in RAM).
 * The loader records where the asset directory begins; a "ROM:name" open SCANS
 * the file for the entry (no in-memory index), then reads it on demand, seek
 * included — exactly like the firmware's rom_find_asset / rom_std_read. */
UTEST(fs, rom_asset_window_read_only_on_demand)
{
    ASSERT_TRUE(fresh_cwd());
    api_errno err;

    /* Build a minimal valid .rp6502: the magic, one program record supplying the
     * reset vector (so emu_rom_load accepts it), then a named asset r.txt="abc".
     * The header's chunks_len marks where the program ends and the asset starts. */
    unsigned char vec[2] = {0x00, 0x80}; /* reset vector bytes at $FFFC/$FFFD */
    uint32_t vcrc = emu_crc32(0, vec, 2);
    char rec[64];
    int recn = snprintf(rec, sizeof(rec), "$FFFC $2 $%X\r\n", vcrc);

    char rompath[300];
    snprintf(rompath, sizeof(rompath), "%s/asset.rp6502", g_dir);
    FILE *rf = fopen(rompath, "wb");
    ASSERT_TRUE(rf != NULL);
    fputs("#!RP6502\r\n", rf);
    fprintf(rf, "#>$%X $0\r\n", (unsigned)(recn + 2)); /* chunks_len = the program section */
    fwrite(rec, 1, (size_t)recn, rf);
    fwrite(vec, 1, 2, rf);
    fputs("#>$3 $0 r.txt\r\n", rf); /* asset directory: r.txt, 3 bytes */
    fwrite("abc", 1, 3, rf);
    fclose(rf);

    ASSERT_TRUE(emu_rom_load(rompath));

    /* Read-only: opening the asset for write is refused. */
    ASSERT_TRUE(std_open("ROM:r.txt", O_WR | O_CREAT_, &err) < 0);
    ASSERT_EQ(err, API_EACCES);

    int f = std_open("ROM:r.txt", O_RD, NULL);
    ASSERT_TRUE(f >= 0);
    ASSERT_FALSE(std_writable(f));
    char buf[8] = {0};
    uint32_t got = 0;
    ASSERT_EQ(std_read(f, buf, 8, &got, &err), STD_OK); /* on-demand read of the window */
    ASSERT_EQ(got, 3u);
    ASSERT_STREQ(buf, "abc");
    /* seek into the window + a partial read still come from the file */
    ASSERT_EQ(std_lseek(f, 1, SEEK_SET), 1L);
    ASSERT_EQ(std_read(f, buf, 1, &got, &err), STD_OK);
    ASSERT_EQ(got, 1u);
    ASSERT_EQ(buf[0], 'b');
    std_close(f);

    ASSERT_TRUE(std_open("ROM:missing.txt", O_RD, &err) < 0);
    ASSERT_EQ(err, API_ENOENT);
}

UTEST_MAIN()
