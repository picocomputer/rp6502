/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for the host-backed filesystem: the MSC0: drive IS the native host
 * filesystem (no chroot — MSC0:/ is the OS root, a relative path resolves the
 * process cwd, ".." walks freely), plus the read-only ROM: drive. Both the file
 * and dir/metadata ops are driven as the 6502 does — stage the xstack, call the
 * std_api_* / hostfs_api_* handler, read AX and any pushed result (stdsys.h,
 * dirsys.h) — since those handlers are now the whole implementation.
 */

#include "emu/api/std.h"
#include "emu/mon/rom.h"
#include "emu/api/hostfs.h"
#include "emu/host/msc.h"
#include "dirsys.h"
#include "stdsys.h"
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
    char resolved[MSC_MAX_PATH]; /* realpath needs a PATH_MAX buffer */
    if (!d || !realpath(d, resolved) || strlen(resolved) >= sizeof(g_dir))
        return false;
    strcpy(g_dir, resolved);
    std_stop(); /* close any files a prior test left open */
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

    int f = ssys_open("hello.txt", O_WR | O_CREAT_ | O_TRUNC_);
    ASSERT_TRUE(f >= 0);
    ASSERT_EQ(ssys_write(f, "hello", 5), 5);
    ssys_close(f);
    ASSERT_TRUE(host_exists("hello.txt")); /* a real file under the cwd */

    f = ssys_open("hello.txt", O_RD);
    ASSERT_TRUE(f >= 0);
    char buf[8] = {0};
    ASSERT_EQ(ssys_read(f, buf, 8), 5);
    ASSERT_STREQ(buf, "hello");
    ASSERT_EQ(ssys_lseek(f, 1, SEEK_SET), 1);
    ASSERT_EQ(ssys_read(f, buf, 1), 1);
    ASSERT_EQ(buf[0], 'e');
    ssys_close(f);

    /* The same file by its MSC0: (relative) path -> the process cwd. */
    f = ssys_open("MSC0:hello.txt", O_RD);
    ASSERT_TRUE(f >= 0);
    ssys_close(f);

    dsys_path("hello.txt");
    hostfs_api_unlink();
    ASSERT_EQ(dsys_ax(), 0);
    ASSERT_FALSE(host_exists("hello.txt"));
}

UTEST(fs, chdir_getcwd_relative)
{
    ASSERT_TRUE(fresh_cwd());

    char cwd[MSC_MAX_PATH], expect[MSC_MAX_PATH];
    hostfs_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    snprintf(expect, sizeof(expect), "MSC0:%s", g_dir); /* getcwd is the native cwd */
    ASSERT_STREQ(cwd, expect);

    dsys_path("saves");
    hostfs_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("saves");
    hostfs_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    hostfs_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    snprintf(expect, sizeof(expect), "MSC0:%s/saves", g_dir);
    ASSERT_STREQ(cwd, expect);

    /* A relative path resolves under the new cwd. */
    int f = ssys_open("game.sav", O_WR | O_CREAT_ | O_TRUNC_);
    ASSERT_TRUE(f >= 0);
    ssys_close(f);
    ASSERT_TRUE(host_exists("saves/game.sav"));

    dsys_path("nope");
    hostfs_api_chdir();
    ASSERT_EQ(dsys_ax(), -1); /* missing dir fails */
}

/* No chroot: MSC0: is the whole native filesystem, so ".." walks freely up the
 * real tree (the old mount-root confinement is gone). */
UTEST(fs, no_chroot_clamp)
{
    ASSERT_TRUE(fresh_cwd());

    dsys_path("sub");
    hostfs_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("sub");
    hostfs_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    char cwd[MSC_MAX_PATH], expect[MSC_MAX_PATH];
    hostfs_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    snprintf(expect, sizeof(expect), "MSC0:%s/sub", g_dir);
    ASSERT_STREQ(cwd, expect);

    /* ".." climbs back to the launch dir ... */
    dsys_path("..");
    hostfs_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    hostfs_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    snprintf(expect, sizeof(expect), "MSC0:%s", g_dir);
    ASSERT_STREQ(cwd, expect);

    /* ... and again climbs ABOVE it — no clamp; the cwd walks the real tree. */
    dsys_path("..");
    hostfs_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    hostfs_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    ASSERT_STRNE(cwd, expect);
}

/* The MSC0:<->host translation: MSC0: maps straight onto the native filesystem
 * (absolute from the OS root); the Windows //C/ form names an explicit drive. */
UTEST(fs, path_translation)
{
    char host[MSC_MAX_PATH], msc[MSC_MAX_PATH];

    ASSERT_TRUE(msc_to_host("MSC0:/sub/file", host, sizeof(host)));
    ASSERT_STREQ(host, "/sub/file");
    ASSERT_TRUE(msc_to_host("0:/sub/file", host, sizeof(host))); /* numeric drive alias */
    ASSERT_STREQ(host, "/sub/file");
    ASSERT_TRUE(msc_to_host("MSC0://C/Users/Homey", host, sizeof(host)));
    ASSERT_STREQ(host, "C:/Users/Homey");

    msc_from_host("/sub/file", msc, sizeof(msc));
    ASSERT_STREQ(msc, "MSC0:/sub/file");
    msc_from_host("C:/Users/Homey", msc, sizeof(msc));
    ASSERT_STREQ(msc, "MSC0://C/Users/Homey"); /* another drive keeps //C/ */
}

UTEST(fs, chdrive_stays_on_msc0)
{
    ASSERT_TRUE(fresh_cwd());
    dsys_path("MSC0:");
    hostfs_api_chdrive();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("MSC0");
    hostfs_api_chdrive();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("Z:");
    hostfs_api_chdrive();
    ASSERT_EQ(dsys_ax(), -1); /* another drive is not a thing */
}

/* FatFs attribute bits the 6502 sees (FatFs AM_*). */
#define AM_RDO 0x01
#define AM_DIR 0x10
#define AM_ARC 0x20

static void make_file(const char *rel, const char *data, uint16_t n)
{
    int f = ssys_open(rel, O_WR | O_CREAT_ | O_TRUNC_);
    if (f >= 0)
    {
        ssys_write(f, data, n);
        ssys_close(f);
    }
}

/* Directory enumeration + stat + free space against a real temp directory. */
UTEST(fs, dir_enumeration)
{
    ASSERT_TRUE(fresh_cwd());
    make_file("alpha.txt", "hello", 5);
    make_file("beta.dat", "wider content here", 18);
    dsys_path("subdir");
    hostfs_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);

    /* stat reports size + synthesized FAT attributes. */
    FILINFO info;
    dsys_path("alpha.txt");
    hostfs_api_stat();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_filinfo(&info);
    ASSERT_EQ(info.fsize, 5u);
    ASSERT_TRUE(info.fattrib & AM_ARC);
    ASSERT_FALSE(info.fattrib & AM_DIR);
    ASSERT_STREQ(info.fname, "alpha.txt");
    dsys_path("subdir");
    hostfs_api_stat();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_filinfo(&info);
    ASSERT_TRUE(info.fattrib & AM_DIR);

    dsys_path("nope.txt");
    hostfs_api_stat();
    ASSERT_EQ(dsys_ax(), -1); /* ENOENT surfaces */

    /* opendir/readdir lists exactly the three entries; "." and ".." are
     * skipped like FatFs; entry order is filesystem-defined, so match by name. */
    dsys_path("");
    hostfs_api_opendir();
    int des = dsys_ax();
    ASSERT_TRUE(des >= 0);
    bool saw_alpha = false, saw_beta = false, saw_sub = false;
    int count = 0;
    for (;;)
    {
        dsys_des(des);
        hostfs_api_readdir();
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

    /* telldir tracks the entry index; rewinddir restarts it. */
    dsys_des(des);
    hostfs_api_telldir();
    ASSERT_EQ(dsys_axsreg(), 3);
    dsys_des(des);
    hostfs_api_rewinddir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_des(des);
    hostfs_api_telldir();
    ASSERT_EQ(dsys_axsreg(), 0);
    dsys_des(des);
    hostfs_api_readdir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_filinfo(&info);
    ASSERT_TRUE(info.fname[0]); /* an entry again after rewind */

    dsys_des(des);
    hostfs_api_closedir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_des(des);
    hostfs_api_readdir();
    ASSERT_EQ(dsys_ax(), -1); /* closed handle -> EBADF */
    dsys_des(99);
    hostfs_api_readdir();
    ASSERT_EQ(dsys_ax(), -1); /* out-of-range -> EINVAL */

    /* getfree reports real host free/total space (in 512-byte sectors). */
    dsys_path("");
    hostfs_api_getfree();
    ASSERT_EQ(dsys_ax(), 0);
    uint32_t freeb = 0, totalb = 0;
    dsys_getfree(&freeb, &totalb);
    ASSERT_GT(totalb, 0u);
    ASSERT_TRUE(freeb <= totalb);

    /* no host volume label: an empty string (length 1 = just the terminator). */
    dsys_path("");
    hostfs_api_getlabel();
    ASSERT_EQ(dsys_ax(), 1);

    /* chmod toggles the read-only bit (the one FAT attribute with a host
     * equivalent), visible back through stat. */
    dsys_chmod(AM_RDO, AM_RDO, "alpha.txt");
    hostfs_api_chmod();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("alpha.txt");
    hostfs_api_stat();
    dsys_filinfo(&info);
    ASSERT_TRUE(info.fattrib & AM_RDO);
    dsys_chmod(AM_RDO, 0, "alpha.txt");
    hostfs_api_chmod();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("alpha.txt");
    hostfs_api_stat();
    dsys_filinfo(&info);
    ASSERT_FALSE(info.fattrib & AM_RDO);

    /* utime sets the modification date (FAT-packed: 1990-03-15). */
    dsys_utime((8 << 11), (10 << 9) | (3 << 5) | 15, "beta.dat");
    hostfs_api_utime();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("beta.dat");
    hostfs_api_stat();
    dsys_filinfo(&info);
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
    api_set_errno_opt(2); /* llvm-mos mapping, so ssys_errno() is decodable */

    /* Build a minimal valid .rp6502: the magic, one program record supplying the
     * reset vector (so rom_load accepts it), then a named asset r.txt="abc".
     * The header's chunks_len marks where the program ends and the asset starts. */
    unsigned char vec[2] = {0x00, 0x80}; /* reset vector bytes at $FFFC/$FFFD */
    uint32_t vcrc = rom_crc32(0, vec, 2);
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

    ASSERT_TRUE(rom_load(rompath));

    /* Read-only: opening the asset for write is refused. */
    ASSERT_TRUE(ssys_open("ROM:r.txt", O_WR | O_CREAT_) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_EACCES));

    int f = ssys_open("ROM:r.txt", O_RD);
    ASSERT_TRUE(f >= 0);
    ASSERT_TRUE(ssys_write(f, "x", 1) < 0); /* the driver has no write slot */
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENOSYS));
    char buf[8] = {0};
    ASSERT_EQ(ssys_read(f, buf, 8), 3); /* on-demand read of the window */
    ASSERT_STREQ(buf, "abc");
    /* seek into the window + a partial read still come from the file */
    ASSERT_EQ(ssys_lseek(f, 1, SEEK_SET), 1);
    ASSERT_EQ(ssys_read(f, buf, 1), 1);
    ASSERT_EQ(buf[0], 'b');
    ssys_close(f);

    ASSERT_TRUE(ssys_open("ROM:missing.txt", O_RD) < 0);
    ASSERT_EQ(ssys_errno(), api_platform_errno(API_ENOENT));
}

UTEST_MAIN()
