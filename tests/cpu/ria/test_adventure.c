/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/vtkeys.h"
#include "core/com/com.h"
#include "core/sys/sys.h"
#include "emu_boot.h"
#include <string.h>

static char cap[1 << 16];
static size_t cap_len;

static void tap(const char *buf, int len)
{
    for (int i = 0; i < len && cap_len < sizeof(cap) - 1; i++)
        cap[cap_len++] = buf[i];
    cap[cap_len] = 0;
}

static bool boot(const char *input)
{
    cap_len = 0;
    cap[0] = 0;
    if (!emu_restart(TEST_FIXTURE))
        return false;
    com_set_tx_tap(tap);
    if (input)
        vtkeys_paste(input);
    return true;
}

static void run_frames(int n)
{
    emu_frames((int)n);
}

UTEST(adventure, intro_banner)
{
    ASSERT_TRUE(boot(NULL));
    run_frames(60);
    com_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(cap, "Colossal Cave Adventure") != NULL);
    ASSERT_TRUE(strstr(cap, "Would you like instructions?") != NULL);
    ASSERT_TRUE(sys_running());
}

UTEST(adventure, opening_room)
{
    ASSERT_TRUE(boot("no\n"));
    run_frames(120);
    com_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(cap, "standing at the end of a road") != NULL);
    ASSERT_TRUE(strstr(cap, "small brick") != NULL);
    ASSERT_TRUE(sys_running());
}

UTEST(adventure, parses_a_command)
{
    ASSERT_TRUE(boot("no\ntake lamp\n"));
    run_frames(200);
    com_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(cap, "I see no lamp here") != NULL);
    ASSERT_TRUE(sys_running());
}

UTEST_MAIN_EMU()
