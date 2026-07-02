/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Gamepad: the HID pad xreg + the 10-byte-per-player pad_xram_t mirror in
 * pad.c. The xram_mirror case pokes pad.c directly (no ROM). The ROM-driven
 * cases run the gamepad tester (gamepad.rp6502), which continuously prints each
 * player's state and labels an unplugged slot "Disconnected". Output is read
 * through the stdout tap rather than the framebuffer: the terminal carries a
 * ~1 Hz cursor blink, so a frame CRC would not be stable frame to frame.
 */

#include "emu/sys/com.h"
#include "emu/hid/pad.h"
#include "emu/mon/rom.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
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

static void cap_reset(void)
{
    cap_len = 0;
    cap[0] = 0;
}

static void run(int n)
{
    for (int i = 0; i < n; i++)
        emu_run_frame();
}

/* The xreg maps a four-player block (10 bytes each) into XRAM and keeps it in
 * sync. Byte 0 carries the dpad (0x0F) plus the connected feature bit (0x80);
 * bytes 2/3 are button0/button1. No ROM needed — this pokes pad.c directly. */
UTEST(gamepad, xram_mirror)
{
    pad_reset();

    /* The block must fit below 0x10000; 40 bytes won't fit above 0xFFD8. */
    ASSERT_FALSE(pad_set_xram(0xFFD9));
    ASSERT_TRUE(pad_set_xram(0xFFD8));
    ASSERT_TRUE(pad_set_xram(0xFF78)); /* an arbitrary in-range address */

    /* Unplugged: the whole 10-byte record reads as zero (no connected bit). */
    for (int i = 0; i < 10; i++)
        ASSERT_EQ(xram[0xFF78 + i], 0);

    pad_connect(0, true);
    ASSERT_EQ(xram[0xFF78 + 0] & 0x80, 0x80);

    pad_hid_set(0, PAD_BTN_DPAD_LEFT, true);
    ASSERT_EQ(xram[0xFF78 + 0] & 0x0F, 0x04); /* dpad-left bit */
    pad_hid_set(0, PAD_BTN_A, true);
    ASSERT_EQ(xram[0xFF78 + 2] & 0x01, 0x01); /* button0: A */
    pad_hid_set(0, PAD_BTN_START, true);
    ASSERT_EQ(xram[0xFF78 + 3] & 0x08, 0x08); /* button1: Start */

    pad_hid_set(0, PAD_BTN_DPAD_LEFT, false);
    ASSERT_EQ(xram[0xFF78 + 0] & 0x0F, 0x00); /* released, connected bit stays */
    ASSERT_EQ(xram[0xFF78 + 0] & 0x80, 0x80);

    /* Player 1 lives at +10 and is independent of player 0. */
    pad_connect(1, true);
    ASSERT_EQ(xram[0xFF78 + 10] & 0x80, 0x80);
    ASSERT_EQ(xram[0xFF78 + 10] & 0x0F, 0x00);

    /* Unplugging blanks the whole record. */
    pad_connect(0, false);
    for (int i = 0; i < 10; i++)
        ASSERT_EQ(xram[0xFF78 + i], 0);

    /* pad_reset unmaps the block: later input must not touch XRAM. */
    xram[0xFF78] = 0xAB;
    pad_reset();
    pad_connect(0, true);
    ASSERT_EQ(xram[0xFF78], 0xAB);
}

/* The tester maps the pad block and continuously redraws each player's state.
 * A connected pad prints its button row (which includes "Select"); an unplugged
 * slot prints "Disconnected" instead. */
UTEST(gamepad, connected_pad_renders)
{
    pad_reset();
    ASSERT_TRUE(emu_rom_load(GAMEPAD_ROM));
    emu_init();
    run(20); /* the ROM maps the pad block and draws four empty slots */

    cap_reset();
    com_set_out_tap(tap);
    run(20);
    com_set_out_tap(NULL);
    ASSERT_TRUE(strstr(cap, "Disconnected") != NULL); /* all four unplugged */
    ASSERT_TRUE(strstr(cap, "Select") == NULL);       /* no connected button row */

    pad_connect(0, true);
    pad_hid_set(0, PAD_BTN_START, true);
    run(10);

    cap_reset();
    com_set_out_tap(tap);
    run(20);
    com_set_out_tap(NULL);
    ASSERT_TRUE(strstr(cap, "Select") != NULL); /* P0 now prints its button row */
    ASSERT_FALSE(emu_cpu_halted);
}

/* An unplugged controller is gated out: input on a pad whose connected bit is
 * clear never reaches XRAM, so the program keeps the slot "Disconnected". */
UTEST(gamepad, disconnected_pad_ignored)
{
    pad_reset();
    ASSERT_TRUE(emu_rom_load(GAMEPAD_ROM));
    emu_init();
    run(20);

    /* No pad_connect: the press rides an unplugged controller. */
    pad_hid_set(0, PAD_BTN_START, true);
    run(20);

    cap_reset();
    com_set_out_tap(tap);
    run(20);
    com_set_out_tap(NULL);
    ASSERT_TRUE(strstr(cap, "Disconnected") != NULL);
    ASSERT_TRUE(strstr(cap, "Select") == NULL); /* the press was ignored */
    ASSERT_FALSE(emu_cpu_halted);
}

UTEST_MAIN()
