/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Integration test for the exec/argv syscalls (pro.c, ops 0x08/0x09) and the
 * MSC0: filesystem. exec.rp6502 prints its argv, and when run with one arg
 * (just argv[0], its own path) it re-execs itself with an extra "Foo" arg; the
 * second run sees argc==2 and prints "Success". This exercises argv passing,
 * loading a program by its MSC0: path, and the frame-boundary CPU restart.
 */

#include "emu/api/pro.h"
#include "emu/sys/com.h"
#include "emu/mon/rom.h"
#include "emu/host/msc.h"
#include "emu/sys/sys.h"
#include "utest.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    for (int i = 0; i < n; i++)
        emu_run_frame();
}

UTEST(exec, reexecs_self_with_arg)
{
    cap_len = 0;
    cap[0] = 0;
    ASSERT_TRUE(emu_rom_load(EXEC_ROM));
    emu_init();

    /* Seed argv[0] = the ROM's own MSC0: path, exactly as main.c does, so the
     * program can re-exec itself. chdir into the ROM's directory (like launching
     * `rp6502-emu exec.rp6502` from that dir); argv[0] is the absolute native
     * MSC0: path and round-trips through the exec resolver. */
    char abs[HOST_MSC_MAX_PATH], msc[HOST_MSC_MAX_PATH], dir[HOST_MSC_MAX_PATH];
    ASSERT_TRUE(realpath(EXEC_ROM, abs) != NULL);
    snprintf(dir, sizeof(dir), "%s", abs);
    char *slash = strrchr(dir, '/');
    ASSERT_TRUE(slash != NULL);
    *slash = 0;
    ASSERT_TRUE(chdir(dir) == 0);
    host_msc_from_host(abs, msc, sizeof(msc)); /* -> "MSC0:<abs path>" */
    pro_set_argv0(msc);

    com_set_tx_tap(tap);
    run_frames(90); /* first run -> exec -> second run -> exit */
    com_set_tx_tap(NULL);

    ASSERT_TRUE(emu_cpu_halted);
    ASSERT_EQ(emu_exit_code, 0);
    /* First run reached the exec, second run received the extra arg and won. */
    ASSERT_TRUE(strstr(cap, "Executing self with arg: Foo") != NULL);
    ASSERT_TRUE(strstr(cap, "argv[1] = Foo") != NULL);
    ASSERT_TRUE(strstr(cap, "Success") != NULL);
}

UTEST_MAIN()
