/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * rtt_hid.c is included rather than linked because its structure is static.
 * The test is the debugger: it reads and writes the structure as RttHid in
 * src/core/build.py does through a probe, and runs the task where the main
 * loop would.
 */

#include "core/hid/hid.h"
#include "core/hid/gamepad.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "core/hid/usage.h"
#include "core/sys/xram.h"
#include "emu_boot.h"

#include "host/pico/rtt_hid.c"

#include <stddef.h>
#include <string.h>

#define AT_PAD 0x1000
#define AT_KEYBOARD 0x2000
#define AT_MOUSE 0x3000

enum
{
    VERB_MOUNT = 1,
    VERB_REPORT,
    VERB_UMOUNT,
    VERB_NOP,
};

/* The devices of the bench, byte for byte: the DESCRIPTOR of HidKeyboard,
 * HidMouse and HidGamepad in src/core/build.py, where each item is named.
 * The reports below are laid out as the pack() of each makes them. */
static const uint8_t keyboard_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
    0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2A,
    0xFF, 0x00, 0x81, 0x00, 0xC0,
};
static const uint8_t mouse_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09,
    0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02,
    0x81, 0x06, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01,
    0x81, 0x06, 0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xC0,
    0xC0,
};
static const uint8_t gamepad_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02, 0x05, 0x01,
    0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65,
    0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00, 0x95, 0x01, 0x81,
    0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x09, 0x33, 0x09,
    0x34, 0x26, 0xFF, 0x00, 0x46, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81,
    0x02, 0xC0,
};

/* What the debugger reads by the structure's own pointer and sizes. */
static uint8_t *box(void)
{
    return rtt_hid.box;
}

static uint32_t head(void)
{
    uint32_t v;
    memcpy(&v, box() + rtt_hid.size + 4, sizeof v);
    return v;
}

/* The hid slot of the device in a seat, or -1. */
static int seat_slot(int seat)
{
    return box()[rtt_hid.size + 8 + seat] - 1;
}

static uint8_t seat_claims(int seat)
{
    return box()[rtt_hid.size + 8 + rtt_hid.seats + seat];
}

/* One message: the bytes, then the ids, then the head, and a pass of the
 * main loop. */
static void send_ids(uint8_t verb, uint8_t seat, const void *data, uint16_t len,
                     uint16_t vendor_id, uint16_t product_id)
{
    uint32_t word = verb | (uint32_t)seat << 8 | (uint32_t)len << 16;
    if (len)
        memcpy(box() + rtt_hid.size - len, data, len);
    memcpy(box() + rtt_hid.size, &vendor_id, 2);
    memcpy(box() + rtt_hid.size + 2, &product_id, 2);
    memcpy(box() + rtt_hid.size + 4, &word, 4);
    rtt_hid_task();
}

static void send(uint8_t verb, uint8_t seat, const void *data, uint16_t len)
{
    send_ids(verb, seat, data, len, 0, 0);
}

static bool key_down(uint8_t keycode)
{
    return xram[AT_KEYBOARD + (keycode >> 3)] & (1u << (keycode & 7));
}

/* A player's ten bytes: dpad, sticks, button0, button1, lx, ly, rx, ry, lt
 * and rt. */
static const volatile uint8_t *pad(int player)
{
    return &xram[AT_PAD + player * 10];
}

/* Every seat is emptied, as RttHid does when it opens. */
static void start(void)
{
    for (int seat = 0; seat < rtt_hid.seats; seat++)
        send(VERB_UMOUNT, (uint8_t)seat, NULL, 0);
    keyboard_init();
    mouse_init();
    gamepad_init();
    tablet_init();
    memset((uint8_t *)xram, 0, sizeof xram);
    keyboard_xreg(AT_KEYBOARD);
    mouse_xreg(AT_MOUSE);
    gamepad_xreg(AT_PAD);
}

/* The places RttHid reads and writes, from the structure's start and from
 * its pointer. */
#define AT(s, field) ((size_t)((const char *)&(s).field - (const char *)&(s)))

UTEST(rtt_hid, the_structure_is_where_the_bench_reads_it)
{
    ASSERT_EQ(memcmp(rtt_hid.id, "RP6502 RTT HID\0", 16), 0);
    ASSERT_EQ(AT(rtt_hid, box), 16u);
    ASSERT_EQ(AT(rtt_hid, size), 16u + sizeof(void *));
    ASSERT_EQ(AT(rtt_hid, seats), 18u + sizeof(void *));
    ASSERT_EQ(rtt_hid.size, 512);
    ASSERT_EQ(rtt_hid.seats, 8);
    ASSERT_EQ(AT(rtt_hid_box, vendor_id), 512u);
    ASSERT_EQ(AT(rtt_hid_box, product_id), 514u);
    ASSERT_EQ(AT(rtt_hid_box, head), 516u);
    ASSERT_EQ(AT(rtt_hid_box, slot), 520u);
    ASSERT_EQ(AT(rtt_hid_box, claims), 528u);
}

UTEST(rtt_hid, a_keyboard_mounts_reports_and_unmounts)
{
    start();
    send(VERB_MOUNT, 0, keyboard_desc, sizeof keyboard_desc);
    ASSERT_EQ(head(), 0u);
    ASSERT_GE(seat_slot(0), 0);
    ASSERT_EQ(seat_claims(0), HID_CLAIM_KEYBOARD);
    ASSERT_EQ(hid_slot_claims(seat_slot(0)), HID_CLAIM_KEYBOARD);

    /* Left Shift and the A key, then the six keys it has room for. */
    static const uint8_t shift_a[] = {0x02, 0, HID_KEY_A, 0, 0, 0, 0, 0};
    send(VERB_REPORT, 0, shift_a, sizeof shift_a);
    ASSERT_EQ(head(), 0u);
    ASSERT_TRUE(key_down(HID_KEY_A));
    ASSERT_TRUE(key_down(HID_KEY_SHIFT_LEFT));
    static const uint8_t six[] = {0, 0, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    send(VERB_REPORT, 0, six, sizeof six);
    for (int i = 2; i < 8; i++)
        ASSERT_TRUE(key_down(six[i]));
    ASSERT_FALSE(key_down(HID_KEY_SHIFT_LEFT));
    static const uint8_t none[8] = {0};
    send(VERB_REPORT, 0, none, sizeof none);
    ASSERT_FALSE(key_down(HID_KEY_A));

    /* A key held at the unmount is let go, and the seat is empty. */
    int slot = seat_slot(0);
    send(VERB_REPORT, 0, shift_a, sizeof shift_a);
    send(VERB_UMOUNT, 0, NULL, 0);
    ASSERT_EQ(head(), 0u);
    ASSERT_EQ(seat_slot(0), -1);
    ASSERT_EQ(seat_claims(0), 0);
    ASSERT_EQ(hid_slot_claims(slot), 0);
    ASSERT_FALSE(key_down(HID_KEY_A));
}

/* A mouse is the mouse driver's, which moves the tablet's pointer too
 * (core/sys/event.h). */
UTEST(rtt_hid, a_mouse_moves_the_mouse_and_the_tablet)
{
    start();
    send(VERB_MOUNT, 0, mouse_desc, sizeof mouse_desc);
    ASSERT_EQ(seat_claims(0), HID_CLAIM_MOUSE);

    uint8_t before[5];
    memcpy(before, (uint8_t *)&xram[AT_MOUSE], sizeof before);
    /* The left and forward buttons, 10 right, 4 up, the wheel up one and
     * the pan left one. Two counts are one of the block's. */
    static const uint8_t moved[] = {0x11, 10, 0, 0xFC, 0xFF, 1, 0xFF};
    send(VERB_REPORT, 0, moved, sizeof moved);
    ASSERT_EQ(xram[AT_MOUSE + 0], 0x11);
    ASSERT_EQ((uint8_t)(xram[AT_MOUSE + 1] - before[1]), 5);
    ASSERT_EQ((uint8_t)(xram[AT_MOUSE + 2] - before[2]), 0xFE);
    ASSERT_EQ((uint8_t)(xram[AT_MOUSE + 3] - before[3]), 1);
    ASSERT_EQ((uint8_t)(xram[AT_MOUSE + 4] - before[4]), 0xFF);

    send(VERB_UMOUNT, 0, NULL, 0);
    ASSERT_EQ(xram[AT_MOUSE + 0], 0);
}

UTEST(rtt_hid, a_gamepad_reports_buttons_hat_sticks_and_triggers)
{
    start();
    send(VERB_MOUNT, 0, gamepad_desc, sizeof gamepad_desc);
    ASSERT_EQ(seat_claims(0), HID_CLAIM_PAD);
    ASSERT_EQ(pad(0)[0], GAMEPAD_FEAT_CONNECTED | GAMEPAD_FEAT_STICKS);

    /* A and Start, the hat up and right, the left stick hard left, and the
     * right trigger pulled, which is R2 too. */
    static const uint8_t held[] = {0x01, 0x08, 1, 0, 128, 128, 128, 0, 255};
    send(VERB_REPORT, 0, held, sizeof held);
    ASSERT_EQ(pad(0)[0], GAMEPAD_FEAT_CONNECTED | GAMEPAD_FEAT_STICKS | 0x09);
    ASSERT_EQ(pad(0)[1], 0x04);
    ASSERT_EQ(pad(0)[2], 0x01);
    ASSERT_EQ(pad(0)[3], 0x08 | 0x02);
    ASSERT_EQ((int8_t)pad(0)[4], -128);
    ASSERT_EQ((int8_t)pad(0)[5], 0);
    ASSERT_EQ((int8_t)pad(0)[6], 0);
    ASSERT_EQ((int8_t)pad(0)[7], 0);
    ASSERT_EQ(pad(0)[8], 0);
    ASSERT_EQ(pad(0)[9], 255);

    /* At rest the hat is 8, which is no direction. */
    static const uint8_t rest[] = {0, 0, 8, 128, 128, 128, 128, 0, 0};
    send(VERB_REPORT, 0, rest, sizeof rest);
    ASSERT_EQ(pad(0)[0], GAMEPAD_FEAT_CONNECTED | GAMEPAD_FEAT_STICKS);
    ASSERT_EQ(pad(0)[1], 0);
    ASSERT_EQ(pad(0)[3], 0);

    send(VERB_UMOUNT, 0, NULL, 0);
    ASSERT_EQ(pad(0)[0], 0);
}

/* The ids go to hid_mount: a DualShock 4's make a pad that says PlayStation. */
UTEST(rtt_hid, a_mount_carries_the_vendor_and_product_ids)
{
    start();
    send_ids(VERB_MOUNT, 0, gamepad_desc, sizeof gamepad_desc, 0x054C, 0x05C4);
    ASSERT_EQ(seat_claims(0), HID_CLAIM_PAD);
    ASSERT_EQ(pad(0)[0] & GAMEPAD_FEAT_TYPE_MASK,
              GAMEPAD_FEAT_TYPE(GAMEPAD_TYPE_PLAYSTATION));
}

UTEST(rtt_hid, a_keyboard_a_mouse_and_two_gamepads_at_once)
{
    start();
    send(VERB_MOUNT, 0, keyboard_desc, sizeof keyboard_desc);
    send(VERB_MOUNT, 1, mouse_desc, sizeof mouse_desc);
    send(VERB_MOUNT, 2, gamepad_desc, sizeof gamepad_desc);
    send(VERB_MOUNT, 3, gamepad_desc, sizeof gamepad_desc);
    for (int a = 0; a < 4; a++)
    {
        ASSERT_GE(seat_slot(a), 0);
        for (int b = 0; b < a; b++)
            ASSERT_NE(seat_slot(a), seat_slot(b));
    }
    ASSERT_EQ(seat_slot(4), -1);

    /* Each report reaches its own device. */
    static const uint8_t key[] = {0, 0, HID_KEY_A, 0, 0, 0, 0, 0};
    static const uint8_t click[] = {0x01, 0, 0, 0, 0, 0, 0};
    static const uint8_t a[] = {0x01, 0, 8, 128, 128, 128, 128, 0, 0};
    static const uint8_t b[] = {0x02, 0, 8, 128, 128, 128, 128, 0, 0};
    send(VERB_REPORT, 0, key, sizeof key);
    send(VERB_REPORT, 1, click, sizeof click);
    send(VERB_REPORT, 2, a, sizeof a);
    send(VERB_REPORT, 3, b, sizeof b);
    ASSERT_TRUE(key_down(HID_KEY_A));
    ASSERT_EQ(xram[AT_MOUSE + 0], 0x01);
    ASSERT_EQ(pad(0)[2], 0x01);
    ASSERT_EQ(pad(1)[2], 0x02);

    /* The first gamepad leaves, and the second stays the second player. */
    send(VERB_UMOUNT, 2, NULL, 0);
    ASSERT_EQ(seat_slot(2), -1);
    ASSERT_EQ(pad(0)[0], 0);
    ASSERT_EQ(pad(1)[2], 0x02);
    ASSERT_TRUE(key_down(HID_KEY_A));
}

/* No driver takes it, or the one that would is full: the message is
 * acknowledged and the seat stays empty. */
UTEST(rtt_hid, a_mount_that_fails_leaves_its_seat_empty)
{
    start();
    static const uint8_t nothing[] = {0x05, 0x01, 0x09, 0x80, 0xA1, 0x01, 0xC0};
    send(VERB_MOUNT, 0, nothing, sizeof nothing);
    ASSERT_EQ(head(), 0u);
    ASSERT_EQ(seat_slot(0), -1);
    ASSERT_EQ(seat_claims(0), 0);

    for (uint8_t seat = 0; seat < KEYBOARD_MAX_KEYBOARDS; seat++)
    {
        send(VERB_MOUNT, seat, keyboard_desc, sizeof keyboard_desc);
        ASSERT_GE(seat_slot(seat), 0);
    }
    send(VERB_MOUNT, KEYBOARD_MAX_KEYBOARDS, keyboard_desc, sizeof keyboard_desc);
    ASSERT_EQ(head(), 0u);
    ASSERT_EQ(seat_slot(KEYBOARD_MAX_KEYBOARDS), -1);
}

/* After a reboot the debugger may still send to a seat that is now empty.
 * The report goes nowhere, and not to the device in hid slot 0. */
UTEST(rtt_hid, a_report_to_an_empty_seat_goes_nowhere)
{
    start();
    send(VERB_MOUNT, 0, keyboard_desc, sizeof keyboard_desc);
    ASSERT_EQ(seat_slot(0), 0);
    static const uint8_t key[] = {0, 0, HID_KEY_A, 0, 0, 0, 0, 0};
    send(VERB_REPORT, 1, key, sizeof key);
    ASSERT_EQ(head(), 0u);
    ASSERT_FALSE(key_down(HID_KEY_A));
}

/* A message as long as there is room for begins at the structure's start. */
UTEST(rtt_hid, a_message_fills_the_data)
{
    start();
    uint8_t desc[RTT_HID_DATA_SIZE] = {0};
    memcpy(desc, keyboard_desc, sizeof keyboard_desc);
    send(VERB_MOUNT, 0, desc, sizeof desc);
    ASSERT_EQ(head(), 0u);
    ASSERT_EQ(seat_claims(0), HID_CLAIM_KEYBOARD);
}

/* The bench times a round trip with a verb that is nothing. */
UTEST(rtt_hid, another_verb_is_only_acknowledged)
{
    start();
    send(VERB_MOUNT, 0, keyboard_desc, sizeof keyboard_desc);
    int slot = seat_slot(0);
    send(VERB_NOP, 0, NULL, 0);
    ASSERT_EQ(head(), 0u);
    ASSERT_EQ(seat_slot(0), slot);
    ASSERT_EQ(seat_claims(0), HID_CLAIM_KEYBOARD);

    /* With no message the task does nothing. */
    rtt_hid_task();
    ASSERT_EQ(seat_slot(0), slot);
}

UTEST_MAIN_EMU()
