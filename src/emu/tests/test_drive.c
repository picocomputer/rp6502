/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The drive backings beyond the plain host MSC0:, exercised on the host:
 *   - installed ROMs on the null drive ":" (--rom): a .rp6502 reached as ":name",
 *     open/load only — resolved for boot/exec and openable read-only, separate
 *     from MSC0:, but never the cwd and never enumerated or stat'd.
 *   - the transparent MSC0: mount (no chroot): the drive is rooted at the launch
 *     dir, absolute MSC0:/ is that mount, and ".." may walk out of it.
 *   - the ephemeral --tmpdrive: MSC0: backed by a fresh throwaway temp dir.
 */

#include "emu/api/api.h"
#include "emu/api/std.h"
#include "emu/mon/install.h"
#include "emu/mon/rom.h"
#include "emu/msc/msc.h"
#include "emu/msc/mscdir.h"
#include "emu/msc/mscpath.h"
#include "utest.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    std_files_reset();
    return fs_set_cwd(g_dir); /* mount MSC0: at the temp dir */
}

static void make_file(const char *rel, const char *data, size_t n)
{
    int f = std_open(rel, O_WR | O_CREAT_ | O_TRUNC_);
    if (f >= 0)
    {
        size_t put;
        std_write(f, data, n, &put);
        std_close(f);
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
    make_file("hello.rp6502", "NOT THE ROM", 11);

    ASSERT_TRUE(fs_install_rom(HELLO_ROM)); /* ":hello.rp6502" -> HELLO_ROM */

    /* A second install coexists on the null drive. */
    make_file("second.rp6502", "#!RP6502 two", 12);
    char second[FS_HOST_MAX_PATH];
    snprintf(second, sizeof(second), "%s/second.rp6502", g_dir);
    ASSERT_TRUE(fs_install_rom(second));

    /* The boot/exec loader resolves ":name" to the backing file — both installs,
     * case-insensitively like the firmware. */
    char host[FS_HOST_MAX_PATH];
    ASSERT_TRUE(fs_resolve_rom(":hello.rp6502", host, sizeof(host)));
    ASSERT_STREQ(host, HELLO_ROM);
    ASSERT_TRUE(fs_resolve_rom(":HELLO.RP6502", host, sizeof(host))); /* case-insensitive */
    ASSERT_STREQ(host, HELLO_ROM);
    ASSERT_TRUE(fs_resolve_rom(":second.rp6502", host, sizeof(host)));
    ASSERT_STREQ(host, second);
    /* An uninstalled or empty ":name" does not resolve. */
    errno = 0;
    ASSERT_FALSE(fs_resolve_rom(":nope.rp6502", host, sizeof(host)));
    ASSERT_FALSE(fs_resolve_rom(":", host, sizeof(host)));

    /* The boot/exec loader streams the installed file. */
    ASSERT_TRUE(emu_rom_load(":hello.rp6502"));

    /* A 6502 open(":name") is NOT a thing — like the firmware it goes to MSC0:,
     * where a leading ":" is refused; the install never leaks to the host fs. */
    errno = 0;
    ASSERT_TRUE(std_open(":hello.rp6502", O_RD) < 0);
    ASSERT_TRUE(std_open(":", O_RD) < 0);

    /* The same basename on MSC0: is the real (different) host file, untouched. */
    int f = std_open("hello.rp6502", O_RD);
    ASSERT_TRUE(f >= 0);
    char buf[8] = {0};
    size_t got = 0;
    ASSERT_EQ(std_read(f, buf, 8, &got), IO_OK);
    ASSERT_EQ(memcmp(buf, "NOT THE ", 8), 0); /* MSC0:, never the install */
    std_close(f);
}

/* The null drive is loader-only: never the cwd, never enumerated/stat'd/mutated.
 * Every MSC0: op on a ":name" (or bare ":") refuses it cleanly, and ":" never
 * aliases a host path — not even via "MSC0::name". */
UTEST(drive, install_null_drive_has_no_cwd_dir_stat)
{
    ASSERT_TRUE(fresh());
    ASSERT_TRUE(fs_install_rom(HELLO_ROM)); /* ":hello.rp6502" */

    fs_info_t info;
    ASSERT_EQ(fs_stat(":hello.rp6502", &info), -1);
    ASSERT_EQ(fs_opendir(":"), -1);
    ASSERT_EQ(fs_chdir(":hello.rp6502"), -1);
    ASSERT_EQ(fs_chdrive(":"), -1); /* not a cwd-able drive */
    ASSERT_EQ(fs_unlink(":hello.rp6502"), -1);
    ASSERT_EQ(fs_mkdir(":sub"), -1);
    /* "MSC0::name" must not alias the null drive onto a host path either. */
    ASSERT_EQ(fs_stat("MSC0::hello.rp6502", &info), -1);
}

/* The MSC0: mount is transparent (no chroot): rooted at the launch dir, absolute
 * MSC0:/ is that mount, and ".." may walk out of it. */
UTEST(drive, mount_transparent_no_chroot)
{
    ASSERT_TRUE(fresh()); /* mount = g_dir */

    /* getcwd reports the mount as MSC0:/ — not the host path. */
    char cwd[FS_HOST_MAX_PATH];
    fs_getcwd(cwd, sizeof(cwd));
    ASSERT_STREQ(cwd, "MSC0:/");

    /* An absolute MSC0:/ path lands at the mount (= g_dir), not the host root. */
    int f = std_open("MSC0:/save.dat", O_WR | O_CREAT_ | O_TRUNC_);
    ASSERT_TRUE(f >= 0);
    std_close(f);
    char hostprobe[512];
    snprintf(hostprobe, sizeof(hostprobe), "%s/save.dat", g_dir);
    FILE *hp = fopen(hostprobe, "rb");
    ASSERT_TRUE(hp != NULL);
    if (hp)
        fclose(hp);

    /* chdir into a subdir; getcwd stays in MSC0: space. */
    ASSERT_EQ(fs_mkdir("MSC0:/sub"), 0);
    ASSERT_EQ(fs_chdir("MSC0:/sub"), 0);
    fs_getcwd(cwd, sizeof(cwd));
    ASSERT_STREQ(cwd, "MSC0:/sub");

    /* ".." climbs back to the mount, then ABOVE it — no confinement (the old
     * --drive-root chroot would have refused this with EACCES). */
    ASSERT_EQ(fs_chdir(".."), 0);
    fs_getcwd(cwd, sizeof(cwd));
    ASSERT_STREQ(cwd, "MSC0:/");
    ASSERT_EQ(fs_chdir(".."), 0);
    fs_getcwd(cwd, sizeof(cwd));
    ASSERT_STRNE(cwd, "MSC0:/"); /* now above the mount */
}

/* --tmpdrive backs MSC0: with a fresh throwaway dir: empty, writable, and full
 * dir+stat (it is the host driver with an ephemeral root). */
UTEST(drive, tmpdrive_is_fresh_and_writable)
{
    std_files_reset();
    ASSERT_TRUE(fs_use_tmpdrive());

    /* getcwd shows the mount as the drive root, not the host temp path. */
    char cwd[FS_HOST_MAX_PATH];
    fs_getcwd(cwd, sizeof(cwd));
    ASSERT_STREQ(cwd, "MSC0:/");

    /* Empty to start: no entries. */
    int des = fs_opendir("MSC0:/");
    ASSERT_TRUE(des >= 0);
    fs_info_t info;
    ASSERT_EQ(fs_readdir(des, &info), 0);
    ASSERT_FALSE(info.name[0]); /* EOF immediately */
    fs_closedir(des);

    /* Write, then see it via stat + readdir. */
    make_file("MSC0:/scratch.dat", "tmp", 3);
    ASSERT_EQ(fs_stat("MSC0:/scratch.dat", &info), 0);
    ASSERT_EQ(info.size, 3u);

    des = fs_opendir("MSC0:/");
    ASSERT_TRUE(des >= 0);
    bool saw = false;
    for (;;)
    {
        if (fs_readdir(des, &info) != 0)
            break;
        if (!info.name[0])
            break;
        if (!strcmp(info.name, "scratch.dat"))
            saw = true;
    }
    fs_closedir(des);
    ASSERT_TRUE(saw);

    /* Re-mount a normal dir so any later test starts clean (the temp dir is
     * removed at process exit). */
    fs_set_cwd("/tmp");
}

/* The windowed real-time path runs data transfers as non-blocking POSIX AIO
 * (msc_set_async): the driver submits the transfer and returns IO_PENDING until
 * it completes. Drive it to completion (the per-scanline RIA pump does this in
 * the running emulator) and check the bytes, that the fd offset tracks across
 * reads, EOF, and lseek interop. Left in sync mode for the other tests. */
static io_result drain_write(int fd, const void *buf, size_t n, size_t *put)
{
    io_result r;
    do
        r = std_write(fd, buf, n, put);
    while (r == IO_PENDING);
    return r;
}

static io_result drain_read(int fd, void *buf, size_t n, size_t *got)
{
    io_result r;
    do
        r = std_read(fd, buf, n, got);
    while (r == IO_PENDING);
    return r;
}

/* The asserting body. A failing ASSERT returns out of here, not out of the
 * UTEST wrapper, so async mode is always restored before the suite continues. */
static void async_aio_body(int *utest_result)
{
    char src[2000];
    for (size_t i = 0; i < sizeof(src); i++)
        src[i] = (char)(i * 7 + 1);

    int fd = std_open("async.dat", O_WR | O_CREAT_ | O_TRUNC_);
    ASSERT_TRUE(fd >= 0);
    size_t put = 0;
    ASSERT_EQ(drain_write(fd, src, sizeof(src), &put), IO_OK);
    ASSERT_EQ(put, sizeof(src));
    std_close(fd);

    fd = std_open("async.dat", O_RD);
    ASSERT_TRUE(fd >= 0);
    char buf[2000] = {0};
    size_t got = 0;
    /* two sequential reads: the fd offset must advance across them */
    ASSERT_EQ(drain_read(fd, buf, 1000, &got), IO_OK);
    ASSERT_EQ(got, 1000u);
    ASSERT_EQ(memcmp(buf, src, 1000), 0);
    ASSERT_EQ(drain_read(fd, buf, 1000, &got), IO_OK);
    ASSERT_EQ(got, 1000u);
    ASSERT_EQ(memcmp(buf, src + 1000, 1000), 0);
    /* EOF: a further read returns zero bytes (aio_return == 0) */
    ASSERT_EQ(drain_read(fd, buf, 1000, &got), IO_OK);
    ASSERT_EQ(got, 0u);
    /* lseek interoperates with the aio offset snapshot */
    ASSERT_EQ(std_lseek(fd, 500, SEEK_SET), 500);
    ASSERT_EQ(drain_read(fd, buf, 16, &got), IO_OK);
    ASSERT_EQ(got, 16u);
    ASSERT_EQ(memcmp(buf, src + 500, 16), 0);
    std_close(fd);
}

UTEST(drive, async_aio_transfer)
{
    ASSERT_TRUE(fresh());
    msc_set_async(true);
    async_aio_body(utest_result);
    msc_set_async(false); /* leave the suite in sync mode for the other tests */
}

UTEST_MAIN()
