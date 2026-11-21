/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/key.h"
#include "core/hid/usage.h"
#include "core/api/proc.h"
#include "core/sys/sst.h"
#include "machine.h"

/* The key whose press last acted as a chord, or 0. */
static uint8_t key_chord_down;

#define KEY_SST(X) \
    X(SKIP, key_chord_down, "host keyboard state, like keymap's repeat")
SST_TABLE(key, KEY_SST)

bool HOST_IN_RAM_FUNC key_chord(uint8_t keycode, uint8_t modifier, bool repeat)
{
    if (repeat)
        return keycode && keycode == key_chord_down;
    const bool ctrl = modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL);
    const bool alt = modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT);
    const bool acted = proc_key_chord(keycode, ctrl, alt);
    key_chord_down = acted ? keycode : 0;
    return acted;
}
