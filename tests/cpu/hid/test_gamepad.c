/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Gamepad: the HID gamepad xreg + the 10-byte-per-player gamepad_xram_t mirror
 * in gamepad.c, poked directly with no ROM in the way. What a running program
 * sees of the same block is gamepad.txt's business; what only C can reach is
 * here — the xreg's bounds, and that gamepad_stop() unmaps.
 */

#include "core/hid/gamepad.h"
#include "emu_boot.h"
#include "core/sys/xram.h"
#include "core/sys/sys.h"

/* The xreg maps a four-player block (10 bytes each) into XRAM and keeps it in
 * sync. Byte 0 carries the dpad (0x0F) plus the status bits, of which only
 * connected (0x80) is set by a plug with no report behind it yet;
 * bytes 2/3 are button0/button1. No ROM needed — this pokes gamepad.c directly. */
UTEST(gamepad, xram_mirror)
{
    gamepad_stop();

    /* The block must fit below 0x10000; 40 bytes won't fit above 0xFFD8. */
    ASSERT_FALSE(gamepad_xreg(0xFFD9));
    ASSERT_TRUE(gamepad_xreg(0xFFD8));
    ASSERT_TRUE(gamepad_xreg(0xFF78)); /* an arbitrary in-range address */

    /* Unplugged: the whole 10-byte record reads as zero (no connected bit). */
    for (int i = 0; i < 10; i++)
        ASSERT_EQ(xram[0xFF78 + i], 0);

    gamepad_connect(0, true, GAMEPAD_TYPE_UNKNOWN, false);
    ASSERT_EQ(xram[0xFF78 + 0] & 0x80, 0x80);

    gamepad_hid_set(0, GAMEPAD_BTN_DPAD_LEFT, true);
    ASSERT_EQ(xram[0xFF78 + 0] & 0x0F, 0x04); /* dpad-left bit */
    gamepad_hid_set(0, GAMEPAD_BTN_A, true);
    ASSERT_EQ(xram[0xFF78 + 2] & 0x01, 0x01); /* button0: A */
    gamepad_hid_set(0, GAMEPAD_BTN_START, true);
    ASSERT_EQ(xram[0xFF78 + 3] & 0x08, 0x08); /* button1: Start */

    gamepad_hid_set(0, GAMEPAD_BTN_DPAD_LEFT, false);
    ASSERT_EQ(xram[0xFF78 + 0] & 0x0F, 0x00); /* released, connected bit stays */
    ASSERT_EQ(xram[0xFF78 + 0] & 0x80, 0x80);

    /* Player 1 lives at +10 and is independent of player 0. */
    gamepad_connect(1, true, GAMEPAD_TYPE_UNKNOWN, false);
    ASSERT_EQ(xram[0xFF78 + 10] & 0x80, 0x80);
    ASSERT_EQ(xram[0xFF78 + 10] & 0x0F, 0x00);

    /* Unplugging blanks the whole record. */
    gamepad_connect(0, false, GAMEPAD_TYPE_UNKNOWN, false);
    for (int i = 0; i < 10; i++)
        ASSERT_EQ(xram[0xFF78 + i], 0);

    /* gamepad_stop unmaps the block: later input must not touch XRAM. */
    xram[0xFF78] = 0xAB;
    gamepad_stop();
    gamepad_connect(0, true, GAMEPAD_TYPE_UNKNOWN, false);
    ASSERT_EQ(xram[0xFF78], 0xAB);
}

UTEST_MAIN_EMU()
