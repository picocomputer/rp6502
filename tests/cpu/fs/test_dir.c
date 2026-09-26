/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/com.h"
#include "core/sys/sys.h"
#include "emu_boot.h"
#include "osal/dir.h"
#include "tb_hostos.h"
#include <stdio.h>
#include <string.h>

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
    char p[TEST_PATH_MAX + 16];
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
    char d[TEST_PATH_MAX];
    ASSERT_TRUE(host_make_tmpdir(d, sizeof(d)));
    write_file(d, "alpha.txt", "hello");
    write_file(d, "beta.dat", "wider content here");
    char sub[TEST_PATH_MAX + 16];
    snprintf(sub, sizeof(sub), "%s/subdir", d);
    api_errno err;
    ASSERT_TRUE(drive_mkdir(sub, &err));
    ASSERT_TRUE(drive_chdir(d, &err));

    ASSERT_TRUE(emu_restart(DIR_ROM));
    cap_len = 0;
    cap[0] = 0;
    com_set_tx_tap(tap);
    for (int i = 0; i < 20 && sys_running(); i++)
        emu_frames(1);
    com_set_tx_tap(NULL);

    ASSERT_FALSE(sys_running());
    ASSERT_TRUE(strstr(cap, "opendir 00\r\n") != NULL);
    ASSERT_TRUE(strstr(cap, "alpha.txt 20 0005\r\n") != NULL);
    ASSERT_TRUE(strstr(cap, "beta.dat 20 0012\r\n") != NULL);
    ASSERT_TRUE(strstr(cap, "subdir 10 0000\r\n") != NULL);
    ASSERT_TRUE(strstr(cap, "readdir 00\r\n") != NULL);
    ASSERT_TRUE(strstr(cap, "closedir 00\r\n") != NULL);
    /* Only the Pico has volume labels, so getlabel returns -1 here. */
    ASSERT_TRUE(strstr(cap, "getlabel FF\r\n") != NULL);
    ASSERT_TRUE(strstr(cap, "getfree 00\r\n") != NULL);
}

UTEST_MAIN_EMU()
