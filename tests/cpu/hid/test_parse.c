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

UTEST_MAIN();
