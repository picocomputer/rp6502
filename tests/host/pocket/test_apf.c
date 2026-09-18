/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * apf.c is included rather than linked because apf_build, apf_changed,
 * apf_mount, apf_umount and apf_report are static. Its MMIO macros are
 * never dereferenced here, since nothing in this file calls apf_task.
 */

#include "core/str/oem.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "core/sys/xram.h"
#include "core/sys/config.h"

#include "host/pocket/sw/apf.c"

#include "utest.h"

#include <string.h>

#define AT_PAD 0x1000
#define AT_KEYBOARD 0x2000
#define AT_MOUSE 0x3000

/* These are the byte offsets of the fields of gamepad_xram_t, which is
 * private to gamepad.c. */
#define GAMEPAD_DPAD 0
#define GAMEPAD_STICKS 1
#define GAMEPAD_BUTTON0 2
#define GAMEPAD_BUTTON1 3
#define GAMEPAD_LX 4
#define GAMEPAD_LY 5
#define GAMEPAD_RX 6
#define GAMEPAD_RY 7
#define GAMEPAD_LT 8
#define GAMEPAD_RT 9

static const uint8_t *gamepad_rec(int player)
{
    return (uint8_t *)&xram[AT_PAD + player * 10];
}

static uint8_t feed(int slot, uint8_t type, uint32_t key, uint32_t joy,
                    uint16_t trig, bool first)
{
    uint8_t report[APF_REPORT_MAX];
    uint8_t len = apf_build(type, key, joy, trig, first, report);
    if (len && (type == APF_TYPE_MOUSE || apf_changed(slot, report, len)))
        apf_report(slot, report, len);
    return len;
}

static void reset_all(void)
{
    for (int slot = 0; slot < MMIO_CONT_SLOTS; slot++)
        apf_umount(slot);
    apf_refresh();
    keyboard_init();
    mouse_init();
    gamepad_init();
    tablet_init();
    char drain[64];
    while (keymap_in_chars(drain, sizeof drain))
        ;
    memset((uint8_t *)xram, 0, 0x10000);
    keyboard_xreg(AT_KEYBOARD);
    mouse_xreg(AT_MOUSE);
    gamepad_xreg(AT_PAD);
}

UTEST(apf, a_gamepad_maps_straight_through)
{
    reset_all();
    apf_mount(0, APF_TYPE_PAD_ANA);

    feed(0, APF_TYPE_PAD_ANA, 0x30000000u, 0x80808080u, 0x0000, false);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_DPAD] & 0x80, 0x80);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_DPAD] & 0x0F, 0);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_BUTTON0], 0);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_BUTTON1], 0);

    static const struct
    {
        uint32_t key;
        uint8_t dpad;
    } dirs[] = {
        {1u << 0, 0x1}, {1u << 1, 0x2}, {1u << 2, 0x4}, {1u << 3, 0x8},
    };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
    {
        feed(0, APF_TYPE_PAD_ANA, 0x30000000u | dirs[i].key, 0x80808080u,
             0x0000, false);
        ASSERT_EQ(gamepad_rec(0)[GAMEPAD_DPAD] & 0x0F, dirs[i].dpad);
        ASSERT_EQ((int8_t)gamepad_rec(0)[GAMEPAD_LX], 0);
        ASSERT_EQ((int8_t)gamepad_rec(0)[GAMEPAD_LY], 0);
        ASSERT_EQ(gamepad_rec(0)[GAMEPAD_STICKS], 0);
    }

    static const struct
    {
        uint32_t key;
        int byte;
        uint8_t bit;
    } buttons[] = {
        {1u << 4, GAMEPAD_BUTTON0, 1u << 0},  /* A      -> index 0 */
        {1u << 5, GAMEPAD_BUTTON0, 1u << 1},  /* B      -> index 1 */
        {1u << 6, GAMEPAD_BUTTON0, 1u << 3},  /* X      -> index 3 */
        {1u << 7, GAMEPAD_BUTTON0, 1u << 4},  /* Y      -> index 4 */
        {1u << 8, GAMEPAD_BUTTON0, 1u << 6},  /* L1     -> index 6 */
        {1u << 9, GAMEPAD_BUTTON0, 1u << 7},  /* R1     -> index 7 */
        {1u << 14, GAMEPAD_BUTTON1, 1u << 2}, /* select -> index 10 */
        {1u << 15, GAMEPAD_BUTTON1, 1u << 3}, /* start  -> index 11 */
        {1u << 12, GAMEPAD_BUTTON1, 1u << 5}, /* L3     -> index 13 */
        {1u << 13, GAMEPAD_BUTTON1, 1u << 6}, /* R3     -> index 14 */
    };
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
    {
        feed(0, APF_TYPE_PAD_ANA, 0x30000000u | buttons[i].key, 0x80808080u,
             0x0000, false);
        int other = buttons[i].byte == GAMEPAD_BUTTON0 ? GAMEPAD_BUTTON1 : GAMEPAD_BUTTON0;
        ASSERT_EQ(gamepad_rec(0)[buttons[i].byte], buttons[i].bit);
        ASSERT_EQ(gamepad_rec(0)[other], 0);
        ASSERT_EQ(gamepad_rec(0)[GAMEPAD_DPAD] & 0x0F, 0);
    }

    /* gamepad.c reports a pressed L2 or R2 button as its trigger at full
     * scale when that trigger's analog value is zero. */
    feed(0, APF_TYPE_PAD_ANA, 0x30000000u | (1u << 10), 0x80808080u, 0x0000,
         false);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_BUTTON1] & 0x01, 0x01);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_LT], 255);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_RT], 0);
    feed(0, APF_TYPE_PAD_ANA, 0x30000000u | (1u << 11), 0x80808080u, 0x0000,
         false);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_BUTTON1] & 0x02, 0x02);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_RT], 255);

    feed(0, APF_TYPE_PAD_ANA, 0x30000000u, 0x40C000FFu, 0x0000, false);
    ASSERT_EQ((int8_t)gamepad_rec(0)[GAMEPAD_LX], 127);  /* joy[7:0]   = 0xFF */
    ASSERT_EQ((int8_t)gamepad_rec(0)[GAMEPAD_LY], -128); /* joy[15:8]  = 0x00 */
    ASSERT_EQ((int8_t)gamepad_rec(0)[GAMEPAD_RX], 64);   /* joy[23:16] = 0xC0 */
    ASSERT_EQ((int8_t)gamepad_rec(0)[GAMEPAD_RY], -64);  /* joy[31:24] = 0x40 */

    feed(0, APF_TYPE_PAD_ANA, 0x30000000u, 0x80808080u, 0x40C0, false);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_LT], 0xC0);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_RT], 0x40);
}

UTEST(apf, a_gamepad_without_analog_is_centered)
{
    reset_all();
    apf_mount(1, APF_TYPE_PAD);
    feed(1, APF_TYPE_PAD, 0x20000000u | (1u << 2), 0xDEADBEEFu, 0xFFFF, false);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_DPAD] & 0x0F, 0x4); /* left */
    ASSERT_EQ((int8_t)gamepad_rec(0)[GAMEPAD_LX], 0);
    ASSERT_EQ((int8_t)gamepad_rec(0)[GAMEPAD_LY], 0);
    ASSERT_EQ((int8_t)gamepad_rec(0)[GAMEPAD_RX], 0);
    ASSERT_EQ((int8_t)gamepad_rec(0)[GAMEPAD_RY], 0);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_LT], 0);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_RT], 0);
}

UTEST(apf, the_status_bits_follow_the_descriptor)
{
    reset_all();

    apf_mount(0, APF_TYPE_PAD_ANA);
    feed(0, APF_TYPE_PAD_ANA, 0x30000000u, 0x80808080u, 0x0000, false);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_DPAD] & 0xF0, 0xC0); /* connected | sticks */

    reset_all();
    apf_mount(0, APF_TYPE_PAD);
    feed(0, APF_TYPE_PAD, 0x20000000u, 0x80808080u, 0x0000, false);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_DPAD] & 0xF0, 0x80); /* connected only */

    reset_all();
    apf_mount(0, APF_TYPE_POCKET);
    feed(0, APF_TYPE_POCKET, 0x10000000u, 0x80808080u, 0x0000, false);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_DPAD] & 0xF0, 0x80 | (GAMEPAD_TYPE_EASTERN << 4));

    feed(0, APF_TYPE_POCKET, 0x10000000u | (1u << 4), 0x80808080u, 0x0000, false);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_BUTTON0], 1u << 0);
}

UTEST(apf, four_gamepads_are_four_players)
{
    reset_all();
    for (int slot = 0; slot < 4; slot++)
        apf_mount(slot, APF_TYPE_PAD_ANA);
    for (int slot = 0; slot < 4; slot++)
        feed(slot, APF_TYPE_PAD_ANA, 0x30000000u | (1u << (4 + slot)),
             0x80808080u, 0x0000, false);
    /* A, B, X and Y are indices 0, 1, 3 and 4. */
    static const uint8_t want[4] = {1u << 0, 1u << 1, 1u << 3, 1u << 4};
    for (int player = 0; player < 4; player++)
    {
        ASSERT_EQ(gamepad_rec(player)[GAMEPAD_DPAD] & 0x80, 0x80);
        ASSERT_EQ(gamepad_rec(player)[GAMEPAD_BUTTON0], want[player]);
    }
}

UTEST(apf, a_keyboard_sets_the_bitmap)
{
    reset_all();
    apf_mount(2, APF_TYPE_KEYBOARD);

    /* Bit 0 of the bitmap is set while no key is pressed. */
    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0, 0, false);
    ASSERT_EQ(xram[AT_KEYBOARD] & 1, 1);

    /* The A key is HID usage 0x04. APF packs the first four codes into
     * joy, most significant byte first, and the last two into trig. */
    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0x04000000u, 0x0000, false);
    ASSERT_EQ(xram[AT_KEYBOARD + (0x04 >> 3)] & (1 << (0x04 & 7)), 1 << (0x04 & 7));
    ASSERT_EQ(xram[AT_KEYBOARD] & 1, 0);

    /* Left shift is bit 1 of the HID modifier byte and usage 0xE1, and
     * the modifier byte arrives in key[15:8]. */
    feed(2, APF_TYPE_KEYBOARD, 0x40000200u, 0x04050607u, 0x0809, false);
    for (uint8_t kc = 0x04; kc <= 0x09; kc++)
        ASSERT_EQ(xram[AT_KEYBOARD + (kc >> 3)] & (1 << (kc & 7)), 1 << (kc & 7));
    ASSERT_EQ(xram[AT_KEYBOARD + (0xE1 >> 3)] & (1 << (0xE1 & 7)), 1 << (0xE1 & 7));

    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0, 0, false);
    ASSERT_EQ(xram[AT_KEYBOARD] & 1, 1);
    ASSERT_EQ(xram[AT_KEYBOARD + (0x04 >> 3)] & (1 << (0x04 & 7)), 0);
}

UTEST(apf, a_keyboard_types_through_its_layout)
{
    reset_all();
    keymap_set_layout_list("US");
    apf_mount(2, APF_TYPE_KEYBOARD);

    char buf[8];
    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0x04000000u, 0x0000, false); /* a */
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)1);
    ASSERT_EQ(buf[0], 'a');

    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0, 0, false);
    feed(2, APF_TYPE_KEYBOARD, 0x40000200u, 0x04000000u, 0x0000, false);
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)1);
    ASSERT_EQ(buf[0], 'A');

    /* HID usage 0x1C is the Y key of a US keyboard, and the German
     * layout maps it to z. */
    keymap_set_layout_list("DE");
    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0, 0, false);
    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0x1C000000u, 0x0000, false);
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)1);
    ASSERT_EQ(buf[0], 'z');
}

/* Lowercase a with tilde is 0xC6 in CP850 and is absent from CP437. */
UTEST(apf, dead_keys_follow_a_code_page_change)
{
    reset_all();
    oem_set_code_page_run(437);
    keymap_set_layout_list("US-INTL");
    apf_mount(2, APF_TYPE_KEYBOARD);

    char buf[8];

    /* Shift with HID usage 0x35 is the tilde, which is a dead key on the
     * US-INTL layout. */
    feed(2, APF_TYPE_KEYBOARD, 0x40000200u, 0x35000000u, 0x0000, false);
    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0, 0, false);
    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0x04000000u, 0x0000, false); /* a */
    size_t n437 = keymap_in_chars(buf, sizeof buf);
    for (size_t i = 0; i < n437; i++)
        ASSERT_NE((unsigned char)buf[i], 0xC6);
    while (keymap_in_chars(buf, sizeof buf))
        ;

    oem_set_code_page_run(850);
    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0, 0, false);
    feed(2, APF_TYPE_KEYBOARD, 0x40000200u, 0x35000000u, 0x0000, false);
    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0, 0, false);
    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0x04000000u, 0x0000, false); /* a */
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)1);
    ASSERT_EQ((unsigned char)buf[0], 0xC6);
}

UTEST(apf, a_mouse_moves_and_clicks)
{
    reset_all();
    apf_mount(3, APF_TYPE_MOUSE);

    /* Each movement is a little-endian field in a register that APF
     * packs most significant byte first, so +0x40 arrives as 0x4000.
     * apf_build zeroes both movements when first is set, so this report
     * changes only the buttons. */
    feed(3, APF_TYPE_MOUSE, 0x50000001u, 0x00014000u, 0x4000, true);
    uint8_t x0 = xram[AT_MOUSE + 1];
    uint8_t y0 = xram[AT_MOUSE + 2];
    ASSERT_EQ(xram[AT_MOUSE + 0], 0x01); /* joy[23:16] is the buttons */

    /* mouse.c publishes each axis at half the device's count, so +0x40
     * advances X by 0x20. */
    feed(3, APF_TYPE_MOUSE, 0x50000200u, 0x00004000u, 0x2000, false);
    ASSERT_EQ(xram[AT_MOUSE + 1], (uint8_t)(x0 + 0x20));
    ASSERT_EQ(xram[AT_MOUSE + 2], (uint8_t)(y0 + 0x10));
    ASSERT_EQ(xram[AT_MOUSE + 0], 0x00);

    /* -0x40 is 0xFFC0, which arrives byte-swapped as 0xC0FF. */
    feed(3, APF_TYPE_MOUSE, 0x50000300u, 0x0000C0FFu, 0xE0FF, false);
    ASSERT_EQ(xram[AT_MOUSE + 1], (uint8_t)(x0 + 0x20 - 0x20));
    ASSERT_EQ(xram[AT_MOUSE + 2], (uint8_t)(y0 + 0x10 - 0x10));

    ASSERT_EQ(xram[AT_MOUSE + 3], 0);
    ASSERT_EQ(xram[AT_MOUSE + 4], 0);
}

UTEST(apf, a_mouse_is_also_a_pointer)
{
    reset_all();
    tablet_xreg(0x4000);
    apf_mount(3, APF_TYPE_MOUSE);

    feed(3, APF_TYPE_MOUSE, 0x50000001u, 0x00000000u, 0x0000, true);
    /* Contact 0's flags byte is at offset 4 of the tablet block. */
    uint8_t before = xram[0x4000 + 4];
    feed(3, APF_TYPE_MOUSE, 0x50000200u, 0x00010001u, 0x0001, false);
    ASSERT_NE(xram[0x4000 + 4], (uint8_t)(before ^ 0xFF));
    /* The first mouse button sets TABLET_FLAG_LEFT, bit 0 of the flags. */
    ASSERT_NE(xram[0x4000 + 4] & 1, 0);
}

UTEST(apf, mapping_a_driver_republishes_what_is_held)
{
    reset_all();
    apf_mount(0, APF_TYPE_PAD_ANA);

    const uint32_t held = 0x30000000u | (1u << 15); /* start */
    feed(0, APF_TYPE_PAD_ANA, held, 0x808000FFu, 0x0000, false);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_BUTTON1] & (1u << 3), 1u << 3);
    ASSERT_EQ((int8_t)gamepad_rec(0)[GAMEPAD_LX], 127);

    gamepad_xreg(AT_PAD);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_BUTTON1] & (1u << 3), 0);
    ASSERT_EQ((int8_t)gamepad_rec(0)[GAMEPAD_LX], 0);

    apf_refresh();
    feed(0, APF_TYPE_PAD_ANA, held, 0x808000FFu, 0x0000, false);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_BUTTON1] & (1u << 3), 1u << 3);
    ASSERT_EQ((int8_t)gamepad_rec(0)[GAMEPAD_LX], 127);
}

UTEST(apf, a_slot_holds_what_its_type_says)
{
    reset_all();

    apf_mount(0, APF_TYPE_KEYBOARD);
    feed(0, APF_TYPE_KEYBOARD, 0x40000000u, 0x04000000u, 0x0000, false);
    ASSERT_EQ(xram[AT_KEYBOARD + (0x04 >> 3)] & (1 << (0x04 & 7)), 1 << (0x04 & 7));
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_DPAD] & 0x80, 0);

    apf_umount(0);
    apf_mount(0, APF_TYPE_PAD_ANA);
    feed(0, APF_TYPE_PAD_ANA, 0x30000000u | (1u << 4), 0x80808080u, 0x0000,
         false);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_DPAD] & 0x80, 0x80);
    ASSERT_EQ(gamepad_rec(0)[GAMEPAD_BUTTON0], 1u << 0);
}

UTEST_MAIN();
