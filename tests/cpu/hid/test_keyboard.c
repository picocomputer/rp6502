/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/keyboard.h"
#include "core/hid/usage.h"
#include "emu_boot.h"
#include "core/sys/xram.h"
#include <string.h>

#define KB_XRAM 0xFF00
#define KEY_A 0x04
#define KEY_B 0x05

/* A boot keyboard: modifier byte, reserved byte, six keycodes. */
static keyboard_connection_t boot_keyboard(void)
{
    keyboard_connection_t desc;
    memset(&desc, 0, sizeof desc);
    desc.valid = true;
    desc.codes_offset = 16;
    desc.codes_count = 6;
    desc.runs[0].bit_pos = 0;
    desc.runs[0].usage_min = HID_KEY_CONTROL_LEFT;
    desc.runs[0].count = 8;
    return desc;
}

static bool xram_key(uint8_t keycode)
{
    return xram[KB_XRAM + (keycode >> 3)] & (1u << (keycode & 7));
}

static void press(int slot, uint8_t keycode)
{
    uint8_t report[8] = {0, 0, keycode, 0, 0, 0, 0, 0};
    keyboard_report(slot, report, sizeof report);
}

/* A key held while its keyboard unplugs is released in XRAM, and bit 0
 * says nothing is down again. */
UTEST(keyboard, a_key_held_at_unplug_is_released)
{
    keyboard_connection_t desc = boot_keyboard();
    ASSERT_TRUE(keyboard_xreg(KB_XRAM));
    ASSERT_TRUE(keyboard_mount(7, &desc, 0, 0));
    press(7, KEY_A);
    ASSERT_TRUE(xram_key(KEY_A));
    ASSERT_FALSE(xram_key(0));
    ASSERT_TRUE(keyboard_umount(7));
    ASSERT_FALSE(xram_key(KEY_A));
    ASSERT_TRUE(xram_key(0));
    keyboard_stop();
}

/* Unplugging one keyboard releases only its keys; another keyboard's held
 * key stays down, and the lock lamps are still published. */
UTEST(keyboard, unplug_keeps_the_other_keyboards_keys)
{
    keyboard_connection_t desc = boot_keyboard();
    ASSERT_TRUE(keyboard_xreg(KB_XRAM));
    ASSERT_TRUE(keyboard_mount(7, &desc, 0, 0));
    ASSERT_TRUE(keyboard_mount(8, &desc, 0, 0));
    press(7, KEY_A);
    press(8, KEY_B);
    uint8_t locks = xram[KB_XRAM] & 0x0E;
    ASSERT_TRUE(keyboard_umount(7));
    ASSERT_FALSE(xram_key(KEY_A));
    ASSERT_TRUE(xram_key(KEY_B));
    ASSERT_FALSE(xram_key(0));
    ASSERT_EQ(xram[KB_XRAM] & 0x0E, locks);
    ASSERT_TRUE(keyboard_umount(8));
    ASSERT_TRUE(xram_key(0));
    keyboard_stop();
}

UTEST_MAIN_EMU()
