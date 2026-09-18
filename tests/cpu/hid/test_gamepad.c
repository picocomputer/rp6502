/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/gamepad.h"
#include "emu_boot.h"
#include "core/sys/xram.h"
#include "core/sys/sys.h"

UTEST(gamepad, xram_mirror)
{
    gamepad_stop();

    /* The block is four 10-byte records, so 0xFFD8 is the highest address
     * that leaves room for all 40 bytes below 0x10000. */
    ASSERT_FALSE(gamepad_xreg(0xFFD9));
    ASSERT_TRUE(gamepad_xreg(0xFFD8));
    ASSERT_TRUE(gamepad_xreg(0xFF78));

    for (int i = 0; i < 10; i++)
        ASSERT_EQ(xram[0xFF78 + i], 0);

    gamepad_connect(0, true, GAMEPAD_TYPE_UNKNOWN, false);
    ASSERT_EQ(xram[0xFF78 + 0] & 0x80, 0x80);

    gamepad_hid_set(0, GAMEPAD_BTN_DPAD_LEFT, true);
    ASSERT_EQ(xram[0xFF78 + 0] & 0x0F, 0x04);
    gamepad_hid_set(0, GAMEPAD_BTN_A, true);
    ASSERT_EQ(xram[0xFF78 + 2] & 0x01, 0x01);
    gamepad_hid_set(0, GAMEPAD_BTN_START, true);
    ASSERT_EQ(xram[0xFF78 + 3] & 0x08, 0x08);

    gamepad_hid_set(0, GAMEPAD_BTN_DPAD_LEFT, false);
    ASSERT_EQ(xram[0xFF78 + 0] & 0x0F, 0x00);
    ASSERT_EQ(xram[0xFF78 + 0] & 0x80, 0x80);

    gamepad_connect(1, true, GAMEPAD_TYPE_UNKNOWN, false);
    ASSERT_EQ(xram[0xFF78 + 10] & 0x80, 0x80);
    ASSERT_EQ(xram[0xFF78 + 10] & 0x0F, 0x00);

    gamepad_connect(0, false, GAMEPAD_TYPE_UNKNOWN, false);
    for (int i = 0; i < 10; i++)
        ASSERT_EQ(xram[0xFF78 + i], 0);

    xram[0xFF78] = 0xAB;
    gamepad_stop();
    gamepad_connect(0, true, GAMEPAD_TYPE_UNKNOWN, false);
    ASSERT_EQ(xram[0xFF78], 0xAB);
}

UTEST_MAIN_EMU()
