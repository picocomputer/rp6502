/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The core/hid/hid.h host functions on a machine that runs in software and has
 * no HID transport of its own: every report the host hands over is forwarded.
 *
 * The core/hid/keyboard.h hooks are empty. The host's system is the keyboard
 * driver, so the host sets key bits with keyboard_set_key, passes the
 * system's locks to keyboard_set_locks and types through core/hid/hostkey.h.
 */

#include "core/hid/hid.h"
#include "core/hid/keyboard.h"

void hid_remapped(void)
{
}

void keyboard_on_mount(uint16_t vendor_id, uint16_t product_id)
{
    (void)vendor_id;
    (void)product_id;
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
