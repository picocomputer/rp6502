/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/gamepad.h"
#include "emu_boot.h"
#include "core/sys/xram.h"
#include <string.h>

#define PAD_SLOT 7

/* A HID gamepad whose button n is bit n - 1 of a two-byte report. */
static gamepad_connection_t numbered_pad(void)
{
    gamepad_connection_t desc;
    memset(&desc, 0, sizeof desc);
    desc.valid = true;
    for (int i = 0; i < GAMEPAD_MAX_BUTTONS; i++)
        desc.button_offsets[i] = i < 16 ? (uint16_t)i : 0xFFFF;
    return desc;
}

static const volatile uint8_t *pad_record(void)
{
    return &xram[0xFF78 + 10 * gamepad_get_player_num(PAD_SLOT)];
}

/* BTN0 and BTN1 as one word, after pad button n alone is pressed. */
static uint16_t pad_press(int n)
{
    uint8_t report[2] = {0, 0};
    report[(n - 1) / 8] = (uint8_t)(1u << ((n - 1) % 8));
    gamepad_report(PAD_SLOT, report, sizeof report);
    return (uint16_t)(pad_record()[2] | pad_record()[3] << 8);
}

static bool pad_mount(uint16_t vendor_id, uint16_t product_id)
{
    gamepad_stop();
    gamepad_xreg(0xFF78);
    gamepad_connection_t desc = numbered_pad();
    return gamepad_mount(PAD_SLOT, &desc, vendor_id, product_id, GAMEPAD_TYPE_UNKNOWN);
}

/* A generic pad keeps the report's order: button n in bit n - 1. */
UTEST(gamepad, a_generic_pad_keeps_its_order)
{
    ASSERT_TRUE(pad_mount(0x2DC8, 0x6001));
    ASSERT_EQ(pad_record()[0] & GAMEPAD_FEAT_TYPE_MASK, 0);
    for (int n = 1; n <= 15; n++)
        ASSERT_EQ(pad_press(n), (uint16_t)(1u << (n - 1)));
    ASSERT_TRUE(gamepad_umount(PAD_SLOT));
}

/* Its buttons are Y, B, A, X, L, R, ZL, ZR, Minus, Plus, L3, R3, Home and
 * Capture, and each lands in the bit of its label, as type 2 requires. */
UTEST(gamepad, a_wired_switch_pad_reports_by_label)
{
    static const uint16_t want[14] = {0x0010, 0x0002, 0x0001, 0x0008, 0x0040,
                                      0x0080, 0x0100, 0x0200, 0x0400, 0x0800,
                                      0x2000, 0x4000, 0x1000, 0x0000};
    ASSERT_TRUE(pad_mount(0x0F0D, 0x00C1));
    ASSERT_EQ(pad_record()[0] & GAMEPAD_FEAT_TYPE_MASK,
              GAMEPAD_FEAT_TYPE(GAMEPAD_TYPE_EASTERN));
    for (int n = 1; n <= 14; n++)
        ASSERT_EQ(pad_press(n), want[n - 1]);
    ASSERT_TRUE(gamepad_umount(PAD_SLOT));
}

/* Its buttons are Triangle, Circle, Cross, Square, L2, R2, L1, R1, Select
 * and Start. */
UTEST(gamepad, a_playstation_classic_pad_reports_by_label)
{
    static const uint16_t want[10] = {0x0010, 0x0002, 0x0001, 0x0008, 0x0100,
                                      0x0200, 0x0040, 0x0080, 0x0400, 0x0800};
    ASSERT_TRUE(pad_mount(0x054C, 0x0CDA));
    ASSERT_EQ(pad_record()[0] & GAMEPAD_FEAT_TYPE_MASK,
              GAMEPAD_FEAT_TYPE(GAMEPAD_TYPE_PLAYSTATION));
    for (int n = 1; n <= 10; n++)
        ASSERT_EQ(pad_press(n), want[n - 1]);
    ASSERT_TRUE(gamepad_umount(PAD_SLOT));
}

/* A stick whose 16-bit X and Y follow two button bytes. */
static void pad_stick(int16_t x, int16_t y)
{
    const uint8_t report[6] = {0, 0, (uint8_t)x, (uint8_t)(x >> 8),
                               (uint8_t)y, (uint8_t)(y >> 8)};
    gamepad_report(PAD_SLOT, report, sizeof report);
}

/* An XInput stick counts up as positive, so its Y is declared with a reversed
 * range. At rest it reads 0, as X does, and full up and full down read -127
 * and 127, each with only its own stick bit. */
UTEST(gamepad, a_reversed_axis_rests_at_zero)
{
    gamepad_stop();
    gamepad_xreg(0xFF78);
    gamepad_connection_t desc = numbered_pad();
    desc.x_offset = 16, desc.x_size = 16, desc.x_min = -32768, desc.x_max = 32767;
    desc.y_offset = 32, desc.y_size = 16, desc.y_min = 32767, desc.y_max = -32768;
    ASSERT_TRUE(gamepad_mount(PAD_SLOT, &desc, 0, 0, GAMEPAD_TYPE_WESTERN));
    pad_stick(0, 0);
    ASSERT_EQ((int8_t)pad_record()[4], 0);
    ASSERT_EQ((int8_t)pad_record()[5], 0);
    ASSERT_EQ(pad_record()[1], 0);
    pad_stick(0, 32767);
    ASSERT_EQ((int8_t)pad_record()[5], -127);
    ASSERT_EQ(pad_record()[1], 0x01); /* left stick up */
    pad_stick(0, -32768);
    ASSERT_EQ((int8_t)pad_record()[5], 127);
    ASSERT_EQ(pad_record()[1], 0x02); /* left stick down */
    ASSERT_TRUE(gamepad_umount(PAD_SLOT));
}

/* A range with an odd count of values, such as -127..127, has its middle on
 * a whole value, and a stick at rest there reads 0. */
UTEST(gamepad, an_odd_range_rests_at_zero)
{
    gamepad_stop();
    gamepad_xreg(0xFF78);
    gamepad_connection_t desc = numbered_pad();
    desc.x_offset = 16, desc.x_size = 16, desc.x_min = -127, desc.x_max = 127;
    desc.y_offset = 32, desc.y_size = 16, desc.y_min = -32767, desc.y_max = 32767;
    ASSERT_TRUE(gamepad_mount(PAD_SLOT, &desc, 0, 0, GAMEPAD_TYPE_WESTERN));
    pad_stick(0, 0);
    ASSERT_EQ((int8_t)pad_record()[4], 0);
    ASSERT_EQ((int8_t)pad_record()[5], 0);
    pad_stick(127, 32767);
    ASSERT_EQ((int8_t)pad_record()[4], 127);
    ASSERT_EQ((int8_t)pad_record()[5], 127);
    pad_stick(-127, -32767);
    ASSERT_EQ((int8_t)pad_record()[4], -128);
    ASSERT_EQ((int8_t)pad_record()[5], -128);
    ASSERT_TRUE(gamepad_umount(PAD_SLOT));
}

UTEST_MAIN_EMU()
