/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The core/hid/hid.h host functions on a machine that runs in software and has
 * no HID transport of its own. No keyboard lock lights are set, nothing
 * enumerates at boot, and every report the host hands over is forwarded.
 *
 * The core/hid/keyboard.h hooks are empty too. The host's OS decoded the key,
 * so the host sets key bits with keyboard_set_key and types through
 * core/hid/hostkey.h.
 */

#include "core/hid/hid.h"
#include "core/hid/keyboard.h"

void hid_set_leds(uint8_t leds)
{
    (void)leds;
}

bool hid_boot_enumerating(void)
{
    return false;
}

void hid_remapped(void)
{
}

void keyboard_on_press(uint8_t modifier, uint8_t keycode)
{
    (void)modifier;
    (void)keycode;
}

void keyboard_on_modifiers(uint8_t modifier)
{
    (void)modifier;
}
