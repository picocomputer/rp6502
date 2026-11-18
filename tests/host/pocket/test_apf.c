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
#include "core/term/cells.h"
#include "core/hid/layout.h"
#include "core/hid/usage.h"
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
    keymap_init();
    mouse_init();
    gamepad_init();
    tablet_init();
    keymap_abandon();
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
    layout_set_list("US");
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
    layout_set_list("DE");
    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0, 0, false);
    feed(2, APF_TYPE_KEYBOARD, 0x40000000u, 0x1C000000u, 0x0000, false);
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)1);
    ASSERT_EQ(buf[0], 'z');
}

/* Ctrl with 2 and with Space types a zero byte, and Ctrl with 3 an ESC, as
 * xterm and the Linux console do. */
UTEST(apf, ctrl_types_what_xterm_types)
{
    reset_all();
    layout_set_list("US");
    apf_mount(2, APF_TYPE_KEYBOARD);

    char buf[8];
    feed(2, APF_TYPE_KEYBOARD, 0x40000100u, 0x1F000000u, 0x0000, false); /* Ctrl+2 */
    feed(2, APF_TYPE_KEYBOARD, 0x40000100u, 0, 0, false);
    feed(2, APF_TYPE_KEYBOARD, 0x40000100u, 0x2C000000u, 0x0000, false); /* Ctrl+Space */
    feed(2, APF_TYPE_KEYBOARD, 0x40000100u, 0, 0, false);
    feed(2, APF_TYPE_KEYBOARD, 0x40000100u, 0x20000000u, 0x0000, false); /* Ctrl+3 */
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)3);
    ASSERT_EQ(memcmp(buf, "\0\0\x1B", 3), 0);
}

UTEST(apf, a_keymap_peek_at_an_offset_takes_nothing)
{
    reset_all();
    layout_set_list("US");
    apf_mount(2, APF_TYPE_KEYBOARD);

    feed(2, APF_TYPE_KEYBOARD, 0x40000100u, 0x20000000u, 0x0000, false); /* Ctrl+3 */
    feed(2, APF_TYPE_KEYBOARD, 0x40000100u, 0, 0, false);
    feed(2, APF_TYPE_KEYBOARD, 0x40000100u, 0x2C000000u, 0x0000, false); /* Ctrl+Space */
    ASSERT_EQ(keymap_in_peek(0), 0x1B);
    ASSERT_EQ(keymap_in_peek(1), 0);
    ASSERT_EQ(keymap_in_peek(2), -1);
    char buf[8];
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)2);
    ASSERT_EQ(memcmp(buf, "\x1B\0", 2), 0);
}

/* Lowercase a with tilde is 0xC6 in CP850 and is absent from CP437. */
UTEST(apf, dead_keys_follow_a_code_page_change)
{
    reset_all();
    oem_set_code_page_run(437);
    layout_set_list("US-INTL");
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

extern uint64_t apf_host_ns;
extern int apf_host_chords;

#define LCTRL KEYBOARD_MODIFIER_LEFTCTRL
#define LSHIFT KEYBOARD_MODIFIER_LEFTSHIFT
#define LALT KEYBOARD_MODIFIER_LEFTALT
#define LGUI KEYBOARD_MODIFIER_LEFTGUI

// A keyboard report: the modifiers held and the one key down, or 0 for none.
static void keys(uint8_t mod, uint8_t usage)
{
    feed(2, APF_TYPE_KEYBOARD, 0x40000000u | ((uint32_t)mod << 8),
         (uint32_t)usage << 24, 0, false);
}

static void keyboard_start(const char *layouts)
{
    reset_all();
    apf_host_ns = 0;
    apf_host_chords = 0;
    layout_set_list(layouts);
    apf_mount(2, APF_TYPE_KEYBOARD);
}

/* Press a key with its modifiers already down, let it go, and check what it
 * typed. */
#define ASSERT_TYPES(mod, usage, want)                               \
    do                                                               \
    {                                                                \
        char typed_[KEYMAP_KEY_QUEUE_SIZE];                          \
        keys(mod, 0);                                                \
        keys(mod, usage);                                            \
        keys(mod, 0);                                                \
        size_t n_ = keymap_in_chars(typed_, sizeof typed_);          \
        ASSERT_EQ(n_, sizeof(want) - 1);                             \
        ASSERT_EQ(memcmp(typed_, want, n_), 0);                      \
    } while (0)

UTEST(apf, enter_tab_escape_and_backspace_ask_no_layout)
{
    keyboard_start("US DE");
    for (int layout = 0; layout < 2; layout++)
    {
        ASSERT_TYPES(0, HID_KEY_ENTER, "\r");
        ASSERT_TYPES(0, HID_KEY_KEYPAD_ENTER, "\r");
        ASSERT_TYPES(0, HID_KEY_TAB, "\t");
        ASSERT_TYPES(0, HID_KEY_ESCAPE, "\x1B");
        ASSERT_TYPES(0, HID_KEY_BACKSPACE, "\x7F");
        ASSERT_TYPES(LSHIFT, HID_KEY_TAB, "\x1B[Z");
        /* The GUI key changes nothing but its own chord. */
        ASSERT_TYPES(LGUI, HID_KEY_ENTER, "\r");
        ASSERT_TYPES(LGUI, HID_KEY_ARROW_UP, "\x1B[A");
        ASSERT_TYPES(LGUI, 0x10, "m");
        ASSERT_TYPES(LGUI | LCTRL, 0x10, "\r");
        /* Ctrl types a character it has no control byte for. */
        ASSERT_TYPES(LCTRL, 0x1E, "1");
        ASSERT_TYPES(LCTRL, HID_KEY_ENTER, "\r");
        ASSERT_TYPES(LCTRL, HID_KEY_BACKSPACE, "\b");
        ASSERT_TYPES(LALT, HID_KEY_ENTER, "\x1B\r");
        ASSERT_TYPES(KEYBOARD_MODIFIER_RIGHTALT, HID_KEY_TAB, "\x1B\t");
        ASSERT_TYPES(LALT, HID_KEY_BACKSPACE, "\x1B\x7F");
        ASSERT_TYPES(LCTRL | LALT, HID_KEY_BACKSPACE, "\x1B\b");
        ASSERT_TYPES(LGUI, HID_KEY_SPACE, "");
    }
}

UTEST(apf, function_and_navigation_keys_send_their_sequences)
{
    keyboard_start("US");
    ASSERT_TYPES(0, HID_KEY_ARROW_UP, "\x1B[A");
    ASSERT_TYPES(LSHIFT, HID_KEY_ARROW_UP, "\x1B[1;2A");
    ASSERT_TYPES(0, HID_KEY_F1, "\x1BOP");
    ASSERT_TYPES(LCTRL | LALT, HID_KEY_F12, "\x1B[24;7~");
    ASSERT_TYPES(0, HID_KEY_DELETE, "\x1B[3~");
    ASSERT_TYPES(0, HID_KEY_SCROLL_LOCK, "");
}

UTEST(apf, the_keypad_follows_num_lock)
{
    keyboard_start("US");
    /* NumLock starts on: digits from the layout, and Shift navigates with
     * the Shift used up. */
    ASSERT_TYPES(0, HID_KEY_KEYPAD_8, "8");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_DECIMAL, ".");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_ADD, "+");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_COMMA, ",");
    ASSERT_TYPES(LSHIFT, HID_KEY_KEYPAD_8, "\x1B[A");
    ASSERT_TYPES(LSHIFT, HID_KEY_KEYPAD_5, "\x1B[E");
    /* A keypad key's character is typed whatever Ctrl says. */
    ASSERT_TYPES(LCTRL, HID_KEY_KEYPAD_DIVIDE, "/");
    ASSERT_TYPES(LCTRL, HID_KEY_KEYPAD_8, "8");

    ASSERT_TYPES(0, HID_KEY_NUM_LOCK, "");
    ASSERT_EQ(xram[AT_KEYBOARD] & 2, 0);
    ASSERT_TYPES(0, HID_KEY_KEYPAD_8, "\x1B[A");
    ASSERT_TYPES(LSHIFT, HID_KEY_KEYPAD_8, "\x1B[1;2A");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_1, "\x1B[F");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_0, "\x1B[2~");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_DECIMAL, "\x1B[3~");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_5, "\x1B[E");
    ASSERT_TYPES(LALT, HID_KEY_KEYPAD_ADD, "+");
    ASSERT_TYPES(LALT, HID_KEY_KEYPAD_ENTER, "\r");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_ADD, "+");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_COMMA, ",");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_ENTER, "\r");
}

/* The terminal's key modes reach the layout machines' keys through the
 * encoder. NumLock starts on. */
UTEST(apf, the_keys_follow_the_terminal_modes)
{
    keyboard_start("US");
    cells_term.app_cursor = true;
    ASSERT_TYPES(0, HID_KEY_ARROW_UP, "\x1BOA");
    ASSERT_TYPES(LSHIFT, HID_KEY_ARROW_UP, "\x1B[1;2A");
    ASSERT_TYPES(LSHIFT, HID_KEY_KEYPAD_8, "\x1BOA");
    cells_term.app_cursor = false;

    cells_term.newline_mode = true;
    ASSERT_TYPES(0, HID_KEY_ENTER, "\r\n");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_ENTER, "\r\n");
    ASSERT_TYPES(LALT, HID_KEY_ENTER, "\x1B\r\n");
    /* Ctrl-M is a character, not the Return key. */
    ASSERT_TYPES(LCTRL, 0x10, "\r");
    cells_term.newline_mode = false;

    /* The application keypad waits for NumLock to go off, or for Shift,
     * which is then used up. */
    cells_term.app_keypad = true;
    ASSERT_TYPES(0, HID_KEY_KEYPAD_8, "8");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_ADD, "+");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_ENTER, "\r");
    ASSERT_TYPES(LSHIFT, HID_KEY_KEYPAD_8, "\x1BOx");
    ASSERT_TYPES(LSHIFT, HID_KEY_KEYPAD_ADD, "\x1BOk");
    ASSERT_TYPES(0, HID_KEY_NUM_LOCK, "");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_8, "\x1BOx");
    ASSERT_TYPES(LSHIFT, HID_KEY_KEYPAD_5, "\x1BO2u");
    /* Plus is xterm's, and a keypad that has a comma sends a VT100's. */
    ASSERT_TYPES(0, HID_KEY_KEYPAD_ADD, "\x1BOk");
    ASSERT_TYPES(LCTRL, HID_KEY_KEYPAD_ADD, "\x1BO5k");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_COMMA, "\x1BOl");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_SUBTRACT, "\x1BOm");
    ASSERT_TYPES(0, HID_KEY_KEYPAD_ENTER, "\x1BOM");
    /* VT52 mode changes only a VT52's keys. */
    cells_term.vt52 = true;
    ASSERT_TYPES(0, HID_KEY_KEYPAD_8, "\x1B?x");
    ASSERT_TYPES(LCTRL, HID_KEY_KEYPAD_ADD, "\x1B?k");
    ASSERT_TYPES(0, HID_KEY_ARROW_UP, "\x1B" "A");
    ASSERT_TYPES(LCTRL, HID_KEY_ARROW_UP, "\x1B" "A");
    ASSERT_TYPES(0, HID_KEY_F1, "\x1BP");
    ASSERT_TYPES(LCTRL, HID_KEY_DELETE, "\x1B[3~");
    ASSERT_TYPES(LALT, 0x04, "\x1B" "a");
    ASSERT_TYPES(LSHIFT, HID_KEY_TAB, "\x1B[Z");
    cells_term.vt52 = false;
    cells_term.app_keypad = false;
}

/* Ctrl-Alt-Delete takes either Delete key, wherever NumLock is. */
UTEST(apf, the_keypad_delete_breaks_too)
{
    keyboard_start("US");
    ASSERT_TYPES(LCTRL | LALT, HID_KEY_KEYPAD_DECIMAL, "");
    ASSERT_EQ(apf_host_chords, 1);
    ASSERT_TYPES(0, HID_KEY_NUM_LOCK, "");
    ASSERT_TYPES(LCTRL | LALT, HID_KEY_KEYPAD_DECIMAL, "");
    ASSERT_EQ(apf_host_chords, 2);
}

UTEST(apf, alt_over_the_keypad_types_a_character_code)
{
    keyboard_start("US");
    char buf[8];
    keys(LALT, 0);
    keys(LALT, HID_KEY_KEYPAD_6);
    keys(LALT, 0);
    keys(LALT, HID_KEY_KEYPAD_5);
    keys(LALT, 0);
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)0);
    keys(0, 0);
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)1);
    ASSERT_EQ(buf[0], 'A');

    /* Enter while a code is being typed belongs to the code and sends
     * nothing. */
    keys(LALT, HID_KEY_KEYPAD_6);
    keys(LALT, HID_KEY_ENTER);
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)0);
    keys(0, 0);
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)1);
    ASSERT_EQ(buf[0], 6);
}

/* The apostrophe is a dead key on the US-INTL layout. */
UTEST(apf, a_dead_key_ends_at_a_key_the_layout_does_not_type)
{
    keyboard_start("US-INTL");
    ASSERT_TYPES(0, 0x34, "");
    ASSERT_TYPES(0, 0x08, "\x82"); /* e acute in CP437 */

    /* Backspace cancels it. */
    ASSERT_TYPES(0, 0x34, "");
    ASSERT_TYPES(0, HID_KEY_BACKSPACE, "");
    ASSERT_TYPES(0, 0x08, "e");

    /* Anything else types it first. */
    ASSERT_TYPES(0, 0x34, "");
    ASSERT_TYPES(0, HID_KEY_ENTER, "'\r");
    ASSERT_TYPES(0, 0x34, "");
    ASSERT_TYPES(0, HID_KEY_TAB, "'\t");
    ASSERT_TYPES(0, 0x34, "");
    ASSERT_TYPES(LALT, HID_KEY_ENTER, "'\x1B\r");
    ASSERT_TYPES(0, 0x34, "");
    ASSERT_TYPES(LALT, HID_KEY_BACKSPACE, "'\x1B\x7F");
    ASSERT_TYPES(0, 0x34, "");
    ASSERT_TYPES(0, HID_KEY_ARROW_LEFT, "'\x1B[D");
    ASSERT_TYPES(0, 0x34, "");
    cells_term.newline_mode = true;
    ASSERT_TYPES(0, HID_KEY_ENTER, "'\r\n");
    cells_term.newline_mode = false;
    ASSERT_TYPES(0, 0x08, "e");
}

UTEST(apf, a_held_key_repeats_until_it_is_let_go)
{
    keyboard_start("US");
    char buf[8];
    keys(0, 0x04); /* a */
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)1);
    apf_host_ns += 499 * 1000000ull;
    keymap_task();
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)0);
    apf_host_ns += 1000000ull;
    keymap_task();
    keymap_task();
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)1);
    ASSERT_EQ(buf[0], 'a');
    apf_host_ns += 30 * 1000000ull;
    keymap_task();
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)1);

    /* Shift going down ends the repeat, because the key that was repeating
     * is no longer the key held. */
    keys(LSHIFT, 0x04);
    apf_host_ns += 1000 * 1000000ull;
    keymap_task();
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)0);

    keys(0, HID_KEY_ARROW_UP);
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)3);
    apf_host_ns += 500 * 1000000ull;
    keymap_task();
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)3);
    ASSERT_EQ(memcmp(buf, "\x1B[A", 3), 0);
    keys(0, 0);
    apf_host_ns += 1000 * 1000000ull;
    keymap_task();
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)0);
}

/* DECARM off: the first press is typed and nothing follows it. Turning it
 * back on does not start a key already held. */
UTEST(apf, nothing_repeats_with_autorepeat_off)
{
    keyboard_start("US");
    char buf[8];
    cells_term.autorepeat = false;
    keys(0, 0x04); /* a */
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)1);
    apf_host_ns += 1000 * 1000000ull;
    keymap_task();
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)0);
    cells_term.autorepeat = true;
    apf_host_ns += 1000 * 1000000ull;
    keymap_task();
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)0);

    /* Turned off while a key repeats, the repeat ends. */
    keys(0, 0);
    keys(0, HID_KEY_ARROW_UP);
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)3);
    apf_host_ns += 500 * 1000000ull;
    keymap_task();
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)3);
    cells_term.autorepeat = false;
    apf_host_ns += 30 * 1000000ull;
    keymap_task();
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)0);
    cells_term.autorepeat = true;
    keys(0, 0);
}

UTEST(apf, lock_keys_and_chords_act_once)
{
    keyboard_start("US DE");
    char buf[8];

    keys(0, HID_KEY_CAPS_LOCK);
    apf_host_ns += 1000 * 1000000ull;
    keymap_task();
    ASSERT_EQ(xram[AT_KEYBOARD] & 4, 4);
    ASSERT_TYPES(0, 0x04, "A");

    /* GUI-Space goes to the next layout of the list, where HID usage 0x1C
     * types z. */
    keys(LGUI, 0);
    keys(LGUI, HID_KEY_SPACE);
    apf_host_ns += 1000 * 1000000ull;
    keymap_task();
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)0);
    ASSERT_TYPES(0, 0x1C, "Z");
    ASSERT_TYPES(LGUI, HID_KEY_SPACE, "");
    ASSERT_TYPES(0, 0x1C, "Y");

    keys(LCTRL | LALT, 0);
    keys(LCTRL | LALT, HID_KEY_DELETE);
    apf_host_ns += 1000 * 1000000ull;
    keymap_task();
    ASSERT_EQ(apf_host_chords, 1);
    ASSERT_EQ(keymap_in_chars(buf, sizeof buf), (size_t)0);
}

UTEST_MAIN();
