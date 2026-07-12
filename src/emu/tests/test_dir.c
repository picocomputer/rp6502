/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Integration test for directory enumeration. dir.rp6502 (the SDK "dir"
 * example) prints the volume label, the cwd, a listing of the current
 * directory (f_opendir/f_readdir/f_closedir + per-entry size/attributes/date),
 * and the free space (f_getfree). Pointed at a temp directory with known
 * contents, it exercises the whole dir syscall path end to end.
 *
 * Output is captured via the terminal tap (like the adventure test) so the
 * assertions are on the program's real text.
 */

#include "emu/sys/com.h"
#include "emu/mon/rom.h"
#include "emu/api/hostfs.h"
#include "emu/plat.h"
#include "emu/sys/cpu.h"
#include "emu/main.h"
#include "utest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* POSIX always has /tmp; Windows doesn't, so ask the OS for its temp dir. */
static const char *test_tmp_base(void)
{
#ifdef _WIN32
    static char buf[512];
    const char *t = getenv("TEMP");
    if (!t || !*t)
        t = getenv("TMP");
    if (!t || !*t)
        return ".";
    size_t i = 0;
    for (; t[i] && i + 1 < sizeof buf; i++)
        buf[i] = t[i] == '\\' ? '/' : t[i];
    buf[i] = 0;
    return buf;
#else
    return "/tmp";
#endif
}

static char cap[1 << 16];
static size_t cap_len;

static void tap(const char *buf, int len)
{
    for (int i = 0; i < len && cap_len < sizeof(cap) - 1; i++)
        cap[cap_len++] = buf[i];
    cap[cap_len] = 0;
}

static void write_file(const char *dir, const char *name, const char *data)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f)
    {
        fwrite(data, 1, strlen(data), f);
        fclose(f);
    }
}

UTEST(dir, lists_directory)
{
    char tmpl[512];
    snprintf(tmpl, sizeof tmpl, "%s/dir_rom_test_XXXXXX", test_tmp_base());
    const char *d = mkdtemp(tmpl);
    ASSERT_TRUE(d != NULL);
    write_file(d, "alpha.txt", "hello");             /* 5 bytes */
    write_file(d, "beta.dat", "wider content here");  /* 18 bytes */
    char sub[512];
    snprintf(sub, sizeof(sub), "%s/subdir", d);
    ASSERT_TRUE(fs_mkdir(sub));

    ASSERT_TRUE(chdir(d) == 0); /* the program lists "" = the cwd */
    ASSERT_TRUE(rom_load(DIR_ROM));
    main_init();
    cap_len = 0;
    cap[0] = 0;
    com_set_tx_tap(tap);
    for (int i = 0; i < 600 && !cpu_halted(); i++)
        main_run_frame();
    com_set_tx_tap(NULL);

    ASSERT_TRUE(cpu_halted()); /* the program ran to completion */

    /* The cwd (PATH line) and all three entries are listed. The cwd shows as the
     * native MSC0:<host path>. */
    ASSERT_TRUE(strstr(cap, "PATH :") != NULL);
    ASSERT_TRUE(strstr(cap, "MSC0:/") != NULL);
    ASSERT_TRUE(strstr(cap, "alpha.txt") != NULL);
    ASSERT_TRUE(strstr(cap, "beta.dat") != NULL);
    ASSERT_TRUE(strstr(cap, "subdir") != NULL);

    /* Attribute column: files are archive (----A), the directory is ---D-. */
    ASSERT_TRUE(strstr(cap, "----A") != NULL);
    ASSERT_TRUE(strstr(cap, "---D-") != NULL);

    /* Free space line printed (f_getfree). */
    ASSERT_TRUE(strstr(cap, "FREE:") != NULL);
    ASSERT_TRUE(strstr(cap, "512 byte blocks") != NULL);
}

UTEST_MAIN()
