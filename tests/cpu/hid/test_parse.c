/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/parse.h"
#include "utest.h"

/* A Logical Maximum of 255 in one byte is 255 while the Logical Minimum is 0
 * or more, as Linux reads it, and not -1. Read as -1, the range would look
 * reversed and a stick at rest would read hard down. */
UTEST(parse, an_unsigned_range_keeps_its_maximum)
{
    static const uint8_t desc[] = {
        0x05, 0x01,       /* Usage Page (Generic Desktop) */
        0x09, 0x05,       /* Usage (Gamepad) */
        0xA1, 0x01,       /* Collection (Application) */
        0x09, 0x30,       /*   Usage (X) */
        0x09, 0x31,       /*   Usage (Y) */
        0x15, 0x00,       /*   Logical Minimum (0) */
        0x25, 0xFF,       /*   Logical Maximum (255) */
        0x75, 0x08,       /*   Report Size (8) */
        0x95, 0x02,       /*   Report Count (2) */
        0x81, 0x02,       /*   Input (Data, Variable, Absolute) */
        0x05, 0x09,       /*   Usage Page (Button) */
        0x19, 0x01,       /*   Usage Minimum (1) */
        0x29, 0x08,       /*   Usage Maximum (8) */
        0x25, 0x01,       /*   Logical Maximum (1) */
        0x75, 0x01,       /*   Report Size (1) */
        0x95, 0x08,       /*   Report Count (8) */
        0x81, 0x02,       /*   Input (Data, Variable, Absolute) */
        0x15, 0x81,       /*   Logical Minimum (-127) */
        0x25, 0x7F,       /*   Logical Maximum (127) */
        0xC0,             /* End Collection */
    };
    hid_parsed_t out;
    hid_parse(desc, sizeof desc, &out);
    ASSERT_TRUE(out.gamepad.valid);
    ASSERT_EQ(out.gamepad.x_min, 0);
    ASSERT_EQ(out.gamepad.x_max, 255);
    ASSERT_EQ(out.gamepad.y_max, 255);
}

/* A signed range still reads its maximum signed. */
UTEST(parse, a_signed_range_reads_signed)
{
    static const uint8_t desc[] = {
        0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
        0x09, 0x30,       /*   Usage (X) */
        0x16, 0x00, 0x80, /*   Logical Minimum (-32768) */
        0x26, 0xFF, 0x7F, /*   Logical Maximum (32767) */
        0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01,
        0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
        0xC0,
    };
    hid_parsed_t out;
    hid_parse(desc, sizeof desc, &out);
    ASSERT_TRUE(out.gamepad.valid);
    ASSERT_EQ(out.gamepad.x_min, -32768);
    ASSERT_EQ(out.gamepad.x_max, 32767);
}

/* Mouse collections as KVMs, virtual machines and touch panels send them:
 * buttons, then X and Y, each either absolute 0..32767 or relative 8-bit. */
#define MOUSE_HEAD(app, id) 0x05, 0x01, 0x09, app, 0xA1, 0x01, 0x85, id, 0x09, 0x01, \
                            0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,          \
                            0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,          \
                            0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01
#define ABS_MOUSE(app, id) MOUSE_HEAD(app, id), 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, \
                           0x15, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02,    \
                           0x81, 0x02, 0xC0, 0xC0
#define REL_MOUSE(id) MOUSE_HEAD(0x02, id), 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,  \
                      0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, \
                      0xC0, 0xC0
/* A touch screen: a tip, then absolute X and Y. */
#define TOUCH(id) 0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x85, id, 0x09, 0x22, 0xA1, 0x02, \
                  0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, \
                  0x95, 0x07, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x26, 0xFF, \
                  0x0F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0xC0, 0xC0

/* A mouse that reports where it is, not how far it moved, is a tablet laid
 * over the display, and neither the mouse's nor a gamepad's. */
UTEST(parse, an_absolute_mouse_is_a_tablet)
{
    static const uint8_t mouse[] = {ABS_MOUSE(0x02, 1)};
    static const uint8_t pointer[] = {ABS_MOUSE(0x01, 1)};
    const uint8_t *descs[] = {mouse, pointer};
    for (int i = 0; i < 2; i++)
    {
        hid_parsed_t out;
        hid_parse(descs[i], sizeof mouse, &out);
        ASSERT_FALSE(out.mouse.valid);
        ASSERT_TRUE(out.tablet.valid);
        ASSERT_FALSE(out.tablet.x_relative);
        ASSERT_TRUE(out.tablet.touchscreen);
        ASSERT_EQ(out.tablet.x_max, 32767);
        ASSERT_FALSE(out.gamepad.valid);
    }
}

UTEST(parse, a_relative_mouse_is_a_mouse_and_a_tablet)
{
    static const uint8_t desc[] = {REL_MOUSE(1)};
    hid_parsed_t out;
    hid_parse(desc, sizeof desc, &out);
    ASSERT_TRUE(out.mouse.valid);
    ASSERT_TRUE(out.tablet.valid);
    ASSERT_TRUE(out.tablet.x_relative);
    ASSERT_FALSE(out.gamepad.valid);
}

/* A KVM may send both. The mouse takes the relative report even when the
 * absolute one comes first, and the tablet the absolute one. */
UTEST(parse, a_kvm_sending_both_gives_each_driver_its_report)
{
    static const uint8_t desc[] = {ABS_MOUSE(0x02, 1), REL_MOUSE(2)};
    hid_parsed_t out;
    hid_parse(desc, sizeof desc, &out);
    ASSERT_TRUE(out.mouse.valid);
    ASSERT_EQ(out.mouse.report_id, 2);
    ASSERT_TRUE(out.tablet.valid);
    ASSERT_EQ(out.tablet.report_id, 1);
    ASSERT_FALSE(out.gamepad.valid);
}

/* A touch screen's copy of its touches as a mouse, for computers that know no
 * touch screens, goes unused. */
UTEST(parse, a_touch_screen_leaves_its_mouse_copy_unused)
{
    static const uint8_t desc[] = {ABS_MOUSE(0x02, 1), TOUCH(2)};
    hid_parsed_t out;
    hid_parse(desc, sizeof desc, &out);
    ASSERT_FALSE(out.mouse.valid);
    ASSERT_TRUE(out.tablet.valid);
    ASSERT_EQ(out.tablet.report_id, 2);
    ASSERT_FALSE(out.gamepad.valid);
}

/* An absolute device that doesn't say what it is stays a gamepad. */
UTEST(parse, an_unnamed_absolute_device_is_a_gamepad)
{
    static const uint8_t desc[] = {ABS_MOUSE(0x08, 1)}; /* Multi-axis Controller */
    hid_parsed_t out;
    hid_parse(desc, sizeof desc, &out);
    ASSERT_FALSE(out.mouse.valid);
    ASSERT_FALSE(out.tablet.valid);
    ASSERT_TRUE(out.gamepad.valid);
}

UTEST_MAIN();
