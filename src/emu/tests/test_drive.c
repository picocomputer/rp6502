/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The drive backings beyond the plain host MSC0:, exercised on the host:
 *   - installed ROMs on the null drive ":" (--rom): a .rp6502 reached as ":name",
 *     open/load only — resolved for boot/exec and openable read-only, separate
 *     from MSC0:, but never the cwd and never enumerated or stat'd.
 *   - the native MSC0: (no chroot): a relative path resolves the process cwd,
 *     absolute MSC0:/ is the OS root, and ".." walks the real tree.
 *   - the ephemeral --tmpdrive: MSC0: backed by a fresh RAM FatFs (the shared
 *     ria/api/fat.c driver), swapped in as the active dir vtable + file driver.
 */

#include "emu/api/std.h"
#include "emu/mon/install.h"
#include "emu/mon/rom.h"
#include "emu/host/dir.h"
#include "emu/host/fs.h"
#include "emu/sys/mem.h"
#include "emu/usb/msc.h"
#include "fatfs/ff.h"
#include "dirsys.h"
#include "stdsys.h"
#include "utest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define O_RD 0x01
#define O_WR 0x02
#define O_CREAT_ 0x10
#define O_TRUNC_ 0x20

static char g_dir[256]; /* a temp dir, made the MSC0: mount */

static bool fresh(void)
{
    char tmpl[] = "/tmp/drive_test_XXXXXX";
    const char *d = mkdtemp(tmpl);
    char resolved[FS_HOST_MAX_PATH];
    if (!d || !realpath(d, resolved) || strlen(resolved) >= sizeof(g_dir))
        return false;
    strcpy(g_dir, resolved);
    std_stop();
    return chdir(g_dir) == 0; /* MSC0: is the process cwd */
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

/* --rom installs a .rp6502 on the null drive, reached as ":name". Like the
 * firmware, ONLY the boot/exec loader resolves it (fs_resolve_rom + emu_rom_load);
 * a 6502 open(":name") is not special — it goes to MSC0: and fails. Installs are
 * separate from MSC0: (a same-named host file is untouched) and coexist. */
UTEST(drive, install_resolve_and_load)
{
    ASSERT_TRUE(fresh());

    /* A real MSC0: file with the same basename — the install must NOT shadow it. */
    make_file("adventure.rp6502", "NOT THE ROM", 11);

    ASSERT_TRUE(fs_install_rom(ADVENTURE_ROM)); /* ":adventure.rp6502" -> ADVENTURE_ROM */

    /* A second install coexists on the null drive. */
    make_file("second.rp6502", "#!RP6502 two", 12);
    char second[FS_HOST_MAX_PATH];
    snprintf(second, sizeof(second), "%s/second.rp6502", g_dir);
    ASSERT_TRUE(fs_install_rom(second));

    /* The boot/exec loader resolves ":name" to the backing file — both installs,
     * case-insensitively like the firmware. */
    char host[FS_HOST_MAX_PATH];
    ASSERT_TRUE(fs_resolve_rom(":adventure.rp6502", host, sizeof(host)));
    ASSERT_STREQ(host, ADVENTURE_ROM);
    ASSERT_TRUE(fs_resolve_rom(":ADVENTURE.RP6502", host, sizeof(host))); /* case-insensitive */
    ASSERT_STREQ(host, ADVENTURE_ROM);
    ASSERT_TRUE(fs_resolve_rom(":second.rp6502", host, sizeof(host)));
    ASSERT_STREQ(host, second);
    /* An uninstalled or empty ":name" does not resolve. */
    ASSERT_FALSE(fs_resolve_rom(":nope.rp6502", host, sizeof(host)));
    ASSERT_FALSE(fs_resolve_rom(":", host, sizeof(host)));

    /* The boot/exec loader streams the installed file. */
    ASSERT_TRUE(emu_rom_load(":adventure.rp6502"));

    /* A 6502 open(":name") is NOT a thing — like the firmware it goes to MSC0:,
     * where a leading ":" is refused; the install never leaks to the host fs. */
    ASSERT_TRUE(ssys_open(":adventure.rp6502", O_RD) < 0);
    ASSERT_TRUE(ssys_open(":", O_RD) < 0);

    /* The same basename on MSC0: is the real (different) host file, untouched. */
    int f = ssys_open("adventure.rp6502", O_RD);
    ASSERT_TRUE(f >= 0);
    char buf[8] = {0};
    ASSERT_EQ(ssys_read(f, buf, 8), 8);
    ASSERT_EQ(memcmp(buf, "NOT THE ", 8), 0); /* MSC0:, never the install */
    ssys_close(f);
}

/* The null drive is loader-only: never the cwd, never enumerated/stat'd/mutated.
 * Every MSC0: op on a ":name" (or bare ":") refuses it cleanly, and ":" never
 * aliases a host path — not even via "MSC0::name". */
UTEST(drive, install_null_drive_has_no_cwd_dir_stat)
{
    ASSERT_TRUE(fresh());
    ASSERT_TRUE(fs_install_rom(ADVENTURE_ROM)); /* ":adventure.rp6502" */

    dsys_path(":adventure.rp6502");
    host_dir_api_stat();
    ASSERT_EQ(dsys_ax(), -1);
    dsys_path(":");
    host_dir_api_opendir();
    ASSERT_EQ(dsys_ax(), -1);
    dsys_path(":adventure.rp6502");
    host_dir_api_chdir();
    ASSERT_EQ(dsys_ax(), -1);
    dsys_path(":"); /* not a cwd-able drive */
    host_dir_api_chdrive();
    ASSERT_EQ(dsys_ax(), -1);
    dsys_path(":adventure.rp6502");
    host_dir_api_unlink();
    ASSERT_EQ(dsys_ax(), -1);
    dsys_path(":sub");
    host_dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), -1);
    /* "MSC0::name" must not alias the null drive onto a host path either. */
    dsys_path("MSC0::adventure.rp6502");
    host_dir_api_stat();
    ASSERT_EQ(dsys_ax(), -1);
}

/* MSC0: IS the native filesystem (no chroot): a relative path resolves the
 * process cwd, absolute MSC0:/ is the OS root, and ".." walks the real tree. */
UTEST(drive, mount_transparent_no_chroot)
{
    ASSERT_TRUE(fresh()); /* cwd = g_dir */

    char cwd[FS_HOST_MAX_PATH], expect[FS_HOST_MAX_PATH];
    host_dir_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    snprintf(expect, sizeof(expect), "MSC0:%s", g_dir); /* getcwd is the native cwd */
    ASSERT_STREQ(cwd, expect);

    /* A relative MSC0: path lands in the cwd (= g_dir). */
    int f = ssys_open("MSC0:save.dat", O_WR | O_CREAT_ | O_TRUNC_);
    ASSERT_TRUE(f >= 0);
    ssys_close(f);
    char hostprobe[512];
    snprintf(hostprobe, sizeof(hostprobe), "%s/save.dat", g_dir);
    FILE *hp = fopen(hostprobe, "rb");
    ASSERT_TRUE(hp != NULL);
    if (hp)
        fclose(hp);

    /* chdir into a subdir; getcwd tracks the native cwd. */
    dsys_path("sub");
    host_dir_api_mkdir();
    ASSERT_EQ(dsys_ax(), 0);
    dsys_path("sub");
    host_dir_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    host_dir_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    snprintf(expect, sizeof(expect), "MSC0:%s/sub", g_dir);
    ASSERT_STREQ(cwd, expect);

    /* ".." climbs back to the launch dir, then ABOVE it — no confinement (the
     * old --drive-root chroot would have refused this with EACCES). */
    dsys_path("..");
    host_dir_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    host_dir_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    snprintf(expect, sizeof(expect), "MSC0:%s", g_dir);
    ASSERT_STREQ(cwd, expect);
    dsys_path("..");
    host_dir_api_chdir();
    ASSERT_EQ(dsys_ax(), 0);
    host_dir_api_getcwd();
    dsys_str(cwd, sizeof(cwd));
    ASSERT_STRNE(cwd, expect); /* now above the launch dir */
}

/* --tmpdrive backs MSC0: with a fresh RAM FatFs: mount swaps the 6502 file
 * syscalls to the shared fat_std_* driver and the dir syscalls to the firmware's
 * dir_api_* (via the OP array), all over the RAM disk. We inspect the volume with
 * FatFs f_* directly and round-trip a file through the std_* file driver. */
UTEST(drive, tmpdrive_is_fresh_ramfs)
{
    std_stop();
    ASSERT_TRUE(emu_ramdrive_mount());
    ASSERT_TRUE(emu_fat_active()); /* the 6502 syscalls now route to the RAM FatFs */

    /* getcwd reports the FatFs volume root, not a host path. */
    char cwd[64];
    ASSERT_EQ(f_getcwd(cwd, sizeof(cwd)), FR_OK);
    ASSERT_EQ(strncmp(cwd, "MSC0:", 5), 0);

    /* Empty to start: the file is not there yet. */
    FILINFO info;
    ASSERT_EQ(f_stat("scratch.dat", &info), FR_NO_FILE);

    /* Write via the FatFs file driver (std_* -> fat_std_* on tmpdrive) ... */
    make_file("scratch.dat", "tmp", 3);

    /* ... and it lands on the RAM FatFs. */
    ASSERT_EQ(f_stat("scratch.dat", &info), FR_OK);
    ASSERT_EQ(info.fsize, 3u);

    /* Read it back through the file driver. */
    int f = ssys_open("scratch.dat", O_RD);
    ASSERT_TRUE(f >= 0);
    char buf[8] = {0};
    ASSERT_EQ(ssys_read(f, buf, 8), 3);
    ASSERT_STREQ(buf, "tmp");
    ssys_close(f);

    /* Deactivate the FatFs backend so later tests use the host filesystem. */
    emu_ramdrive_unmount();
}

/* The windowed real-time path runs data transfers as non-blocking POSIX AIO
 * (host_set_async): the driver submits the transfer and returns STD_PENDING
 * until it completes; ssys_dispatch re-polls like the per-scanline RIA pump.
 * Drive the xram transfers (the AIO lands straight in xram[]; a read spans
 * multiple 2048-byte chunks) and check the bytes, that the fd offset tracks
 * across reads, EOF, and lseek interop. Left in sync mode for the other tests. */
static void async_aio_body(int *utest_result)
{
    char src[5000];
    for (size_t i = 0; i < sizeof(src); i++)
        src[i] = (char)(i * 7 + 1);

    memcpy(&xram[0x1000], src, sizeof(src));
    int fd = ssys_open("async.dat", O_WR | O_CREAT_ | O_TRUNC_);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ssys_write_xram(fd, 0x1000, sizeof(src)), (int)sizeof(src));
    ssys_close(fd);

    fd = ssys_open("async.dat", O_RD);
    ASSERT_TRUE(fd >= 0);
    /* two sequential reads: the fd offset must advance across them */
    ASSERT_EQ(ssys_read_xram(fd, 0x8000, 3000), 3000);
    ASSERT_EQ(memcmp(&xram[0x8000], src, 3000), 0);
    ASSERT_EQ(ssys_read_xram(fd, 0x8000, 3000), 2000); /* short read at EOF */
    ASSERT_EQ(memcmp(&xram[0x8000], src + 3000, 2000), 0);
    /* EOF: a further read returns zero bytes (aio_return == 0) */
    ASSERT_EQ(ssys_read_xram(fd, 0x8000, 1000), 0);
    /* lseek interoperates with the aio offset snapshot; xstack reads too */
    ASSERT_EQ(ssys_lseek(fd, 500, SEEK_SET), 500);
    char buf[16];
    ASSERT_EQ(ssys_read(fd, buf, 16), 16);
    ASSERT_EQ(memcmp(buf, src + 500, 16), 0);
    ssys_close(fd);
}

UTEST(drive, async_aio_transfer)
{
    ASSERT_TRUE(fresh());
    host_set_async(true);
    async_aio_body(utest_result);
    host_set_async(false); /* leave the suite in sync mode for the other tests */
}

UTEST_MAIN()
