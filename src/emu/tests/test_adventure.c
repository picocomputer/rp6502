/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Integration test for the second milestone: Colossal Cave Adventure. It
 * exercises everything the hello world test does not — ROM-asset files
 * (ROM:/advent*.txt) opened and read through the std file syscalls, and
 * interactive stdin driven through the vendored line editor (rln.c) from
 * injected keystrokes.
 *
 * Output is captured via the terminal tap rather than rendered, so the
 * assertions are on the program's actual text and survive font/term changes.
 */

#include "emu/sys/com.h"
#include "emu/mon/rom.h"
#include "emu/sys/cpu.h"
#include "emu/main.h"
#include "sys/com.h"
#include "utest.h"
#include <string.h>

static char cap[1 << 16];
static size_t cap_len;

static void tap(const char *buf, int len)
{
    for (int i = 0; i < len && cap_len < sizeof(cap) - 1; i++)
        cap[cap_len++] = buf[i];
    cap[cap_len] = 0;
}

static void feed(const char *s)
{
    for (const char *p = s; *p; p++)
        com_kbd_push_byte((uint8_t)(*p == '\n' ? '\r' : *p));
}

static bool boot(const char *input)
{
    cap_len = 0;
    cap[0] = 0;
    if (!rom_load(ADVENTURE_ROM))
        return false;
    main_init();
    com_set_tx_tap(tap);
    if (input)
        feed(input);
    return true;
}

static void run_frames(int n)
{
    for (int i = 0; i < n; i++)
        main_run_frame();
}

/* The intro banner prints before any input is read — proves the program
 * starts and stdout reaches the terminal. */
UTEST(adventure, intro_banner)
{
    ASSERT_TRUE(boot(NULL));
    run_frames(60);
    com_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(cap, "Colossal Cave Adventure") != NULL);
    ASSERT_TRUE(strstr(cap, "Would you like instructions?") != NULL);
    ASSERT_FALSE(cpu_halted()); /* blocked on the first stdin read */
}

/* Answering the first prompt requires a full stdin line read through rln; the
 * room description that follows is read from the ROM:/advent*.txt assets. */
UTEST(adventure, opening_room)
{
    ASSERT_TRUE(boot("no\n"));
    run_frames(120);
    com_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(cap, "standing at the end of a road") != NULL);
    ASSERT_TRUE(strstr(cap, "small brick") != NULL);
    ASSERT_FALSE(cpu_halted());
}

/* A second command proves the parser (which scans the asset vocabulary files)
 * and multi-line stdin both keep working past the first turn. */
UTEST(adventure, parses_a_command)
{
    ASSERT_TRUE(boot("no\ntake lamp\n"));
    run_frames(200);
    com_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(cap, "I see no lamp here") != NULL);
    ASSERT_FALSE(cpu_halted());
}

UTEST_MAIN()
