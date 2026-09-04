/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Integration test for the exec/argv syscalls (proc.c, ops 0x08/0x09) and the
 * MSC0: filesystem. exec.rp6502 prints its argv, and when run with one arg
 * (just argv[0], its own path) it re-execs itself with an extra "Foo" arg; the
 * second run sees argc==2 and prints "Success". This exercises argv passing,
 * loading a program by its MSC0: path, and the frame-boundary CPU restart.
 */

#include "osal/dir.h"
#include "core/sys/proc.h"
#include "core/com/com.h"
#include "osal/fs.h"
#include "core/str/path.h"
#include "osal/os.h"
#include "core/wdc/resb.h"
#include "emu_boot.h"
#include "tb_hostos.h"
#include <stdlib.h>
#include <string.h>

static char cap[1 << 16];
static size_t cap_len;

static void tap(const char *buf, int len)
{
    for (int i = 0; i < len && cap_len < sizeof(cap) - 1; i++)
        cap[cap_len++] = buf[i];
    cap[cap_len] = 0;
}

static void run_frames(int n)
{
    emu_frames((int)n);
}

/* Setup goes through the drive, because that is now the only way in: these
 * are the backend's own slots, called the way core/api/dir.c calls them. */
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

UTEST(exec, reexecs_self_with_arg)
{
    cap_len = 0;
    cap[0] = 0;
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));

    /* Seed argv[0] = the ROM's own MSC0: path, exactly as main.c does, so the
     * program can re-exec itself. chdir into the ROM's directory (like launching
     * `rp6502-emu exec.rp6502` from that dir); realpath answers in the 6502's
     * spelling, which is what round-trips through the exec resolver. */
    char *abs = os_dir_realpath(TEST_FIXTURE);
    ASSERT_TRUE(abs != NULL);
    char dir[TEST_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", abs);
    char *slash = strrchr(dir, '/');
    ASSERT_TRUE(slash != NULL);
    *slash = 0;
    ASSERT_TRUE(drive_chdir_to(dir));
    proc_set_argv(abs, 0, NULL);
    free(abs);

    com_set_tx_tap(tap);
    run_frames(90); /* first run -> exec -> second run -> exit */
    com_set_tx_tap(NULL);

    ASSERT_FALSE(resb_running());
    ASSERT_EQ(proc_get_exit_code(), 0);
    /* First run reached the exec, second run received the extra arg and won. */
    ASSERT_TRUE(strstr(cap, "Executing self with arg: Foo") != NULL);
    ASSERT_TRUE(strstr(cap, "argv[1] = Foo") != NULL);
    ASSERT_TRUE(strstr(cap, "Success") != NULL);
}

UTEST(exec, boot_args_reach_program)
{
    cap_len = 0;
    cap[0] = 0;
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));

    /* Boot args (the CLI's `exec.rp6502 -- Foo`): proc_set_argv resolves the raw
     * host path to MSC0: form itself. argc==2 at startup, so the program prints
     * its argv and wins on the first run, without the re-exec. */
    char *args[] = {"Foo"};
    ASSERT_TRUE(proc_set_argv(TEST_FIXTURE, 1, args));

    com_set_tx_tap(tap);
    run_frames(90);
    com_set_tx_tap(NULL);

    ASSERT_FALSE(resb_running());
    ASSERT_EQ(proc_get_exit_code(), 0);
    ASSERT_TRUE(strstr(cap, "argv[1] = Foo") != NULL);
    ASSERT_TRUE(strstr(cap, "Success") != NULL);
    ASSERT_TRUE(strstr(cap, "Executing self") == NULL);
}

UTEST_MAIN_EMU()
